#pragma once
#include "Skills.h"
#include "AgentProfiles.h"
#include "Lsp.h"

namespace iiMultimodalAgent::agent {
struct PluginStoreOptions {
    QString directory; // Explicit private host directory; never inferred from a project or HOME.
    int maxPlugins = 64;
    int maxEntries = 8192;
    qint64 maxPackageBytes = 128 * 1024 * 1024;
    int maxFileBytes = 8 * 1024 * 1024;
};
struct IIMULTIMODALAGENT_EXPORT PluginInfo {
    QString name, version, description, sha256, root, dataDirectory;
    bool enabled = true;
    QString status; // disabled, blocked, or configured (not a running-server claim).
    QStringList dependencies, blockedBy, unsupportedFeatures;
    QJsonObject contributions;
    QJsonObject toJson(bool includePaths = false) const;
};
struct PluginHookConfiguration {
    QString pluginName, root, dataDirectory;
    QJsonObject settings;
};
// Value snapshot. Store changes apply only to the next snapshot/host start.
// Older cache revisions and persistent data are retained on update/uninstall.
struct IIMULTIMODALAGENT_EXPORT PluginSnapshot {
    QString revision, storeDirectory;
    QList<PluginInfo> plugins;
    QList<SkillSource> skills;
    QList<AgentProfileSource> agents;
    QList<PluginHookConfiguration> hooks;
    QJsonObject mcpServers;
    QMap<QString, QMap<QString, QString>> mcpVariables;
    LspOptions lsp;
    QJsonObject toJson() const; // No commands, prompts, environment, credentials or local paths.
};
class IIMULTIMODALAGENT_EXPORT PluginStore {
public:
    explicit PluginStore(PluginStoreOptions);
    PluginInfo install(const QString& sourceDirectory, bool enabled = true, const CancellationToken& = {});
    void setEnabled(const QString& name, bool enabled);
    void uninstall(const QString& name); // Removes the selection, retains cache and data.
    QJsonArray list() const;
    PluginSnapshot snapshot(const CancellationToken& = {}) const;
    QString directory() const;
private:
    PluginStoreOptions options;
};
}
