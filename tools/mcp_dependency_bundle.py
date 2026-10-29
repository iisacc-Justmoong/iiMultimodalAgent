"""Standard zipimport artifacts for an owned, pinned test dependency environment."""
from pathlib import Path
import hashlib,importlib.util,json,os,stat,sys,tempfile,zipfile
CACHE='.ii-sdk-mcp-cache'
LOADER='_ii_sdk_mcp_bundle.py'
PTH='_ii_sdk_mcp_bundle.pth'
FORMAT=1

def stamp(path):
    info=path.lstat()
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):raise ValueError('Nonregular cache input')
    return [info.st_ino,info.st_size,info.st_mtime_ns,stat.S_IFMT(info.st_mode)]

def atomic_bytes(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix='.prepare-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as stream:stream.write(data)
        os.replace(name,path)
    finally:
        if os.path.exists(name):os.unlink(name)

def build(prefix,library,max_bytes=64*1024*1024):
    prefix=Path(prefix).resolve();library=Path(library).resolve()
    if not library.is_relative_to(prefix):raise ValueError('Library outside managed prefix')
    cache=library/CACHE
    if cache.is_symlink():raise ValueError('Cache directory is a symlink')
    cache.mkdir(exist_ok=True)
    files={};directories={};entries={};excluded=[];unsafe=[];modules=[]
    for top in sorted(library.iterdir()):
        if top.name.startswith('.') or top.name in ('__pycache__','pip','setuptools','_distutils_hack',LOADER) or top.is_symlink():continue
        if top.is_dir():
            if not (top/'__init__.py').is_file():continue
            sources=[];dirs=[];native=False;nonregular=False
            for directory,children,names in os.walk(top,followlinks=False):
                current=Path(directory);dirs.append(current)
                nonregular|=any((current/name).is_symlink() for name in children)
                children[:]=[name for name in children if name!='__pycache__' and not (current/name).is_symlink()]
                for name in names:
                    path=current/name
                    if path.is_symlink() or not path.is_file():nonregular=True;continue
                    if path.suffix.lower() in ('.so','.pyd','.dll','.dylib'):native=True
                    sources.append(path)
            if native:excluded.append(top.name);continue
            if nonregular:unsafe.append(top.name);continue
            for directory in dirs:directories[directory.relative_to(library).as_posix()]=stamp(directory)
        elif top.suffix=='.py':sources=[top];modules.append(top.stem)
        else:continue
        for source in sources:
            relative=source.relative_to(library).as_posix();files[relative]=stamp(source);entries[relative]=source
            if source.suffix=='.py':
                compiled=source.parent/'__pycache__'/(source.stem+'.'+sys.implementation.cache_tag+'.pyc')
                if compiled.is_file() and not compiled.is_symlink() and compiled.resolve().is_relative_to(prefix):
                    files[compiled.relative_to(library).as_posix()]=stamp(compiled)
                    entries[str(Path(relative).with_suffix('.pyc')).replace(os.sep,'/')]=compiled
    if sum(source.stat().st_size for source in entries.values())>max_bytes:
        (library/PTH).unlink(missing_ok=True);return {'status':'too_large'}
    fd,temporary=tempfile.mkstemp(prefix='.bundle-',suffix='.zip',dir=cache);os.close(fd)
    try:
        with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_STORED) as archive:
            for name,source in sorted(entries.items()):archive.write(source,name)
        digest=hashlib.sha256(Path(temporary).read_bytes()).hexdigest()
        os.replace(temporary,cache/'dependencies.zip')
    finally:
        if os.path.exists(temporary):os.unlink(temporary)
    manifest={'format':FORMAT,'cache_tag':sys.implementation.cache_tag,'optimize':sys.flags.optimize,'files':files,'directories':directories,'modules':modules,'excluded_native_packages':excluded,'archive_sha256':digest}
    atomic_bytes(cache/'manifest.json',(json.dumps(manifest,separators=(',',':'))+'\n').encode())
    atomic_bytes(library/LOADER,Path(__file__).read_bytes())
    line='import _ii_sdk_mcp_bundle; _ii_sdk_mcp_bundle.activate('+ascii(str(library))+')\n'
    atomic_bytes(library/PTH,line.encode())
    return {'status':'bundled','sources':len(files),'archive_entries':len(entries),'excluded_native_packages':excluded,'excluded_nonregular_packages':unsafe}

def activate(library=None):
    """Fail closed to normal filesystem imports, without changing package source."""
    library=Path(library or Path(__file__).resolve().parent).resolve();cache=library/CACHE;archive=cache/'dependencies.zip'
    try:
        manifest=json.loads((cache/'manifest.json').read_text())
        if manifest['format']!=FORMAT or manifest['cache_tag']!=sys.implementation.cache_tag or manifest['optimize']!=sys.flags.optimize:return False
        for group in ('files','directories'):
            for relative,expected in manifest[group].items():
                path=Path(relative)
                if path.is_absolute() or '..' in path.parts or stamp(library/path)!=expected:return False
        if any((library/name).is_dir() for name in manifest['modules']):return False
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=manifest['archive_sha256']:return False
        archive=str(archive)
        if archive not in sys.path:
            index=next((i for i,path in enumerate(sys.path) if path and Path(path).resolve()==library),len(sys.path))
            sys.path.insert(index,archive)
        return True
    except (OSError,ValueError,KeyError,TypeError):return False
