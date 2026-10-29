#pragma once
#include <iiMultimodalAgent/agent/Types.h>
#include <QtCore/QUrl>
namespace iiMultimodalAgent::agent::detail {
QString webContent(const QByteArray&,const QByteArray& contentType,const QUrl&,int maxCharacters,
    bool& truncated,const CancellationToken&);
QString webExcerpt(QString,int maxCharacters);
}
