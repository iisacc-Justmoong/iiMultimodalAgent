#include <iiMultimodalAgent/agent/ImageModelProfile.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>

namespace iiMultimodalAgent::agent::image_detail {
namespace {
constexpr quint64 jsonLimit = 1024 * 1024;
constexpr quint64 headerLimit = 64 * 1024 * 1024;
constexpr quint64 centralLimit = 4 * 1024 * 1024;

QString pathText(const std::filesystem::path& path) {
    const auto utf8=path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),qsizetype(utf8.size()));
}
QJsonObject object(const QByteArray& bytes) {
    if (bytes.isEmpty() || quint64(bytes.size())>headerLimit) return {};
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(bytes,&error);
    return error.error==QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}
QJsonObject embeddedObject(const QJsonValue& value) {
    if (value.isObject()) return value.toObject();
    if (value.isString()) {
        const auto bytes=value.toString().toUtf8();
        if (quint64(bytes.size())<=headerLimit) return object(bytes);
    }
    return {};
}
std::optional<int> bounded(const QJsonValue& value,int minimum,int maximum,int multiple=1) {
    if (!value.isDouble()) return {};
    const auto number=value.toDouble();
    if (!std::isfinite(number) || number<minimum || number>maximum || number!=int(number) || int(number)%multiple) return {};
    return int(number);
}
enum class Family { Unknown, MossImage, Krea2, Conflict };
class ProfileBuilder {
public:
    explicit ProfileBuilder(int maximumDimension):maximumDimension_(maximumDimension) {}
    void identity(const QString& value) {
        // Names in explicit metadata fields are permitted; external filenames,
        // descriptions and unrelated component names are never classifiers.
        auto name=value.section('/',-1).toLower();
        QString normalized;
        for (const auto c:name) if (c.isLetterOrNumber()) normalized+=c;
        Family found=Family::Unknown;
        if (normalized=="mossimage" || normalized=="mossimagebase" || normalized=="mossimagesft" ||
            normalized=="mossimagepipeline" || normalized=="mossimagetransformer") found=Family::MossImage;
        else if (normalized=="krea2" || normalized=="krea2pipeline" || normalized=="krea2transformer") found=Family::Krea2;
        if (found!=Family::Unknown) {
            if (family_==Family::Unknown) family_=found;
            else if (family_!=found) family_=Family::Conflict;
        }
    }
    void generation(const QJsonObject& value,bool dedicated=false) {
        // Ignore invalid package hints rather than bypassing the host's limits.
        auto steps=bounded(value.value("num_inference_steps"),1,150);
        if (!steps) steps=bounded(value.value("steps"),1,150);
        if (steps && (dedicated || !dedicatedSteps_)) {
            profile_.steps=steps;profile_.stepsFromPackage=true;if(dedicated) dedicatedSteps_=true;
        }
        if (auto width=bounded(value.value("width"),8,maximumDimension_,8);width && (dedicated || !dedicatedWidth_)) {
            profile_.width=width;if(dedicated) dedicatedWidth_=true;
        }
        if (auto height=bounded(value.value("height"),8,maximumDimension_,8);height && (dedicated || !dedicatedHeight_)) {
            profile_.height=height;if(dedicated) dedicatedHeight_=true;
        }
    }
    void metadata(const QJsonObject& value) {
        for (const auto* key:{"_class_name","architecture","model_type","model_family","family",
            "base_model","modelspec.architecture","modelspec.base_model","iild.model_family","general.architecture"})
            if (value.value(QLatin1String(key)).isString()) identity(value.value(QLatin1String(key)).toString());
        for (const auto& item:value.value("architectures").toArray()) if (item.isString()) identity(item.toString());
        const auto transformer=value.value("transformer").toArray();
        if (transformer.size()==2 && transformer[1].isString()) identity(transformer[1].toString());
        const auto config=embeddedObject(value.value("config"));
        if (!config.isEmpty()) configMetadata(config);
        generation(embeddedObject(value.value("generation_config")));
        generation(embeddedObject(value.value("generation_parameters")));
        generation(embeddedObject(value.value("iild.generation_parameters")));
    }
    void configMetadata(const QJsonObject& value) {
        // This nested object is parsed only one level deep.
        for (const auto* key:{"_class_name","architecture","model_type","model_family","family","base_model"})
            if (value.value(QLatin1String(key)).isString()) identity(value.value(QLatin1String(key)).toString());
        for (const auto& item:value.value("architectures").toArray()) if (item.isString()) identity(item.toString());
        generation(embeddedObject(value.value("generation_config")));
        generation(embeddedObject(value.value("generation_parameters")));
    }
    ImageModelProfile finish() {
        if (family_==Family::MossImage) {
            profile_.family="moss-image";
            if (!profile_.steps) profile_.steps=8;
        } else if (family_==Family::Krea2) {
            profile_.family="krea2";
            if (!profile_.steps) profile_.steps=52;
        }
        return profile_;
    }
private:
    int maximumDimension_;
    Family family_=Family::Unknown;
    ImageModelProfile profile_;
    bool dedicatedSteps_=false,dedicatedWidth_=false,dedicatedHeight_=false;
};

quint64 le(const QByteArray& bytes,qsizetype offset,int size,bool& ok) {
    if (offset<0 || size<0 || size>8 || offset>bytes.size()-size) {ok=false;return 0;}
    quint64 result=0;
    for(int i=0;i<size;++i) result|=quint64(static_cast<unsigned char>(bytes[offset+i]))<<(i*8);
    return result;
}
QByteArray readRange(QFile& file,quint64 offset,quint64 count) {
    const auto size=file.size();
    if (size<0 || offset>quint64(size) || count>quint64(size)-offset || count>headerLimit ||
        offset>quint64(std::numeric_limits<qint64>::max()) || !file.seek(qint64(offset))) return {};
    const auto result=file.read(qint64(count));
    return quint64(result.size())==count ? result : QByteArray{};
}
using Reader=std::function<QByteArray(quint64,quint64)>;

// GGUF metadata is sequential, preceding tensor descriptors and data. Parse it
// with a fixed byte/work budget and do not inspect general.name or filenames.
void gguf(ProfileBuilder& profile,const Reader& read,quint64 size) {
    struct Cursor {
        const Reader& read;quint64 size,pos=0;bool ok=true;quint64 work=0;
        QByteArray take(quint64 count) {
            if(!ok || pos>size || count>size-pos || pos>headerLimit || count>headerLimit-pos) {ok=false;return {};}
            auto bytes=read(pos,count);if(quint64(bytes.size())!=count) ok=false;pos+=count;return bytes;
        }
        quint64 number(int count) {const auto bytes=take(count);return le(bytes,0,count,ok);}
        QByteArray string() {const auto length=number(8);if(length>jsonLimit){ok=false;return {};}
            return take(length);}
        void skip(unsigned type,unsigned depth=0) {
            if(!ok || depth>4 || ++work>100000) {ok=false;return;}
            if(type==8){(void)string();return;}
            if(type==9){const auto element=number(4),count=number(8);
                if(count>100000-work){ok=false;return;}
                for(quint64 i=0;i<count && ok;++i) skip(unsigned(element),depth+1);return;}
            static constexpr unsigned widths[]{1,1,2,2,4,4,4,1,0,0,8,8,8};
            if(type>=std::size(widths) || !widths[type]) {ok=false;return;}
            (void)take(widths[type]);
        }
    } cursor{read,size};
    if(cursor.take(4)!="GGUF") return;
    const auto version=cursor.number(4);(void)cursor.number(8);const auto fields=cursor.number(8);
    if(!cursor.ok || (version!=2 && version!=3) || fields>16384) return;
    QJsonObject metadata;
    for(quint64 i=0;i<fields && cursor.ok;++i) {
        const auto rawKey=cursor.string();const auto type=cursor.number(4);
        if(!cursor.ok || rawKey.isEmpty()) return;
        if(type==8) metadata.insert(QString::fromUtf8(rawKey),QString::fromUtf8(cursor.string()));
        else cursor.skip(unsigned(type));
    }
    if(cursor.ok) profile.metadata(metadata);
}

// Safetensors headers contain metadata and, in iiLocalDiffusion's packaged
// format, offsets for individual configuration assets. Never read tensors.
void safetensors(ProfileBuilder& profile,const Reader& read,quint64 size) {
    if(size<10) return;
    bool ok=true;const auto prefix=read(0,8);const auto length=le(prefix,0,8,ok);
    if(!ok || length<2 || length>headerLimit || length>size-8) return;
    const auto header=object(read(8,length));
    if(header.isEmpty()) return;
    const auto metadata=header.value("__metadata__").toObject();
    profile.metadata(metadata);
    const auto manifest=embeddedObject(metadata.value("iild_package_manifest"));
    profile.metadata(manifest);
    const auto files=manifest.value("files").toArray();
    if(files.size()<=10000) for(const auto& entry:files) {
        const auto source=entry.toObject();const auto status=source.value("status").toString();
        if(source.value("role").toString()=="model" && (status=="included" || status=="duplicate"))
            profile.metadata(embeddedObject(source.value("metadata")));
    }
    for(auto it=header.begin();it!=header.end();++it) {
        constexpr auto prefixName="__iild_assets__/";
        if(!it.key().startsWith(QLatin1String(prefixName))) continue;
        const auto remainder=it.key().mid(int(std::char_traits<char>::length(prefixName)));
        const auto slash=remainder.indexOf('/');if(slash<0) continue;
        const auto name=remainder.mid(slash+1);
        const bool config=name=="model_index.json" || name=="config.json" || name=="transformer/config.json";
        if(!config && name!="generation_config.json") continue;
        const auto info=it.value().toObject();const auto offsets=info.value("data_offsets").toArray();
        const auto shape=info.value("shape").toArray();
        if(info.value("dtype")!="U8" || offsets.size()!=2 || !offsets[0].isDouble() || !offsets[1].isDouble()) continue;
        const auto begin=offsets[0].toInteger(-1),end=offsets[1].toInteger(-1);
        if(begin<0 || end<begin || shape.size()!=1 || shape[0].toInteger(-1)!=end-begin ||
            quint64(end-begin)>jsonLimit || quint64(end)>size-8-length) continue;
        const auto contents=object(read(8+length+quint64(begin),quint64(end-begin)));
        if(config) profile.metadata(contents); else profile.generation(contents,true);
    }
}

bool safeMember(const QString& name) {
    if(name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':') || name.contains(QChar(0))) return false;
    const auto parts=name.split('/');
    return std::none_of(parts.begin(),parts.end(),[](const QString& part){return part.isEmpty() || part=="." || part=="..";});
}
struct ZipEntry {quint64 offset=0,size=0;};
class StoredZip {
public:
    explicit StoredZip(QFile& file):file_(file) { parse(); }
    bool valid() const {return valid_;}
    quint64 size(const QString& name) const {return entries_.value(name).size;}
    QByteArray read(const QString& name,quint64 offset,quint64 count) {
        const auto found=entries_.constFind(name);
        if(found==entries_.cend() || offset>found->size || count>found->size-offset) return {};
        return readRange(file_,found->offset+offset,count);
    }
private:
    void parse() {
        const auto fileSize=file_.size();if(fileSize<22) return;
        const auto tailSize=std::min<quint64>(quint64(fileSize),65557);
        const auto tail=readRange(file_,quint64(fileSize)-tailSize,tailSize);
        bool ok=true;qsizetype end=-1;
        for(qsizetype i=tail.size()-22;i>=0;--i) {
            if(le(tail,i,4,ok)==0x06054b50 && i+22+qsizetype(le(tail,i+20,2,ok))==tail.size()) {end=i;break;}
        }
        if(!ok || end<0 || le(tail,end+4,2,ok)!=0 || le(tail,end+6,2,ok)!=0) return;
        auto count=le(tail,end+10,2,ok),diskCount=le(tail,end+8,2,ok);
        auto directorySize=le(tail,end+12,4,ok),directoryOffset=le(tail,end+16,4,ok);
        const auto endOffset=quint64(fileSize)-tailSize+quint64(end);
        if(count==0xffff || diskCount==0xffff || directorySize==0xffffffff || directoryOffset==0xffffffff) {
            if(endOffset<20) return;
            const auto locator=readRange(file_,endOffset-20,20);
            if(le(locator,0,4,ok)!=0x07064b50 || le(locator,4,4,ok)!=0 || le(locator,16,4,ok)!=1) return;
            const auto zip64=readRange(file_,le(locator,8,8,ok),56);
            if(le(zip64,0,4,ok)!=0x06064b50 || le(zip64,4,8,ok)<44 ||
                le(zip64,16,4,ok)!=0 || le(zip64,20,4,ok)!=0) return;
            diskCount=le(zip64,24,8,ok);count=le(zip64,32,8,ok);
            directorySize=le(zip64,40,8,ok);directoryOffset=le(zip64,48,8,ok);
        }
        if(!ok || !count || count>1024 || count!=diskCount || directorySize>centralLimit ||
            directoryOffset>endOffset || directorySize>endOffset-directoryOffset) return;
        const auto directory=readRange(file_,directoryOffset,directorySize);
        if(quint64(directory.size())!=directorySize) return;
        qsizetype pos=0;
        for(quint64 index=0;index<count;++index) {
            if(le(directory,pos,4,ok)!=0x02014b50) return;
            const auto flags=le(directory,pos+8,2,ok),method=le(directory,pos+10,2,ok);
            auto compressed=le(directory,pos+20,4,ok),uncompressed=le(directory,pos+24,4,ok);
            const auto nameSize=le(directory,pos+28,2,ok),extraSize=le(directory,pos+30,2,ok),commentSize=le(directory,pos+32,2,ok);
            const auto disk=le(directory,pos+34,2,ok);auto localOffset=le(directory,pos+42,4,ok);
            if(!ok || (flags&1) || method!=0 || disk!=0 || nameSize==0 ||
                quint64(pos)>directorySize || 46+nameSize+extraSize+commentSize>directorySize-quint64(pos)) return;
            const auto name=QString::fromUtf8(directory.constData()+pos+46,qsizetype(nameSize));
            if(!safeMember(name) || entries_.contains(name)) return;
            const auto extraBegin=pos+46+qsizetype(nameSize),extraEnd=extraBegin+qsizetype(extraSize);
            bool foundZip64=false;
            for(auto extra=extraBegin;extra+4<=extraEnd;) {
                const auto id=le(directory,extra,2,ok),length=le(directory,extra+2,2,ok);extra+=4;
                if(!ok || length>quint64(extraEnd-extra)) return;
                if(id==1) {
                    foundZip64=true;auto cursor=extra;
                    auto next=[&]() {if(cursor+8>extra+qsizetype(length)){ok=false;return quint64(0);}auto v=le(directory,cursor,8,ok);cursor+=8;return v;};
                    if(uncompressed==0xffffffff) uncompressed=next();
                    if(compressed==0xffffffff) compressed=next();
                    if(localOffset==0xffffffff) localOffset=next();
                }
                extra+=qsizetype(length);
            }
            if(!ok || ((!foundZip64) && (uncompressed==0xffffffff || compressed==0xffffffff || localOffset==0xffffffff)) ||
                compressed!=uncompressed || localOffset>=directoryOffset) return;
            const auto local=readRange(file_,localOffset,30);
            if(le(local,0,4,ok)!=0x04034b50 || le(local,6,2,ok)!=flags || le(local,8,2,ok)!=method) return;
            const auto localName=le(local,26,2,ok),localExtra=le(local,28,2,ok);
            if(!ok || localName!=nameSize || localOffset>directoryOffset ||
                30+localName+localExtra>directoryOffset-localOffset) return;
            if(readRange(file_,localOffset+30,localName)!=name.toUtf8()) return;
            const auto dataOffset=localOffset+30+localName+localExtra;
            if(uncompressed>directoryOffset-dataOffset) return;
            entries_.insert(name,{dataOffset,uncompressed});
            pos+=qsizetype(46+nameSize+extraSize+commentSize);
        }
        valid_=ok && quint64(pos)==directorySize;
    }
    QFile& file_;QHash<QString,ZipEntry> entries_;bool valid_=false;
};
void zip(ProfileBuilder& profile,QFile& file) {
    StoredZip archive(file);if(!archive.valid()) return;
    QJsonObject manifest;
    for(const auto* raw:{"model_index.json","config.json","transformer/config.json","generation_config.json"}) {
        const QString name=QLatin1String(raw);const auto size=archive.size(name);
        if(!size || size>jsonLimit) continue;
        const auto value=object(archive.read(name,0,size));
        if(name=="generation_config.json") profile.generation(value,true);else profile.metadata(value);
        if(name=="model_index.json") manifest=value;
    }
    const auto stages=manifest.value("stages").toArray();
    if(stages.isEmpty()) return;
    const auto name=stages.first().toObject().value("model").toString();
    if(!safeMember(name)) return;
    const auto size=archive.size(name);if(size<10) return;
    const Reader reader=[&](quint64 offset,quint64 count){return archive.read(name,offset,count);};
    if(reader(0,4)=="GGUF") gguf(profile,reader,size);else safetensors(profile,reader,size);
}
}

ImageModelProfile inspectImageModel(const std::filesystem::path& source,int maximumDimension) {
    ProfileBuilder profile(maximumDimension);
    const auto path=pathText(source);const QFileInfo info(path);
    if(info.isDir()) {
        const auto root=info.canonicalFilePath();
        if(root.isEmpty()) return profile.finish();
        for(const auto* raw:{"model_index.json","config.json","transformer/config.json","generation_config.json"}) {
            const QString name=QLatin1String(raw);QFile file(QDir(root).filePath(name));const QFileInfo member(file);
            const auto canonical=member.canonicalFilePath();
            if(canonical.isEmpty() || !canonical.startsWith(root+QLatin1Char('/')) || !member.isFile() ||
                member.size()<0 || quint64(member.size())>jsonLimit || !file.open(QIODevice::ReadOnly)) continue;
            const auto value=object(file.readAll());
            if(name=="generation_config.json") profile.generation(value,true);else profile.metadata(value);
        }
    } else if(info.isFile()) {
        QFile file(path);
        if(file.open(QIODevice::ReadOnly) && file.size()>=4) {
            const auto magic=readRange(file,0,4);
            if(magic==QByteArray("PK\003\004",4)) zip(profile,file);
            else {
                const Reader reader=[&](quint64 offset,quint64 count){return readRange(file,offset,count);};
                if(magic=="GGUF") gguf(profile,reader,quint64(file.size()));
                else safetensors(profile,reader,quint64(file.size()));
            }
        }
    }
    return profile.finish();
}
}
