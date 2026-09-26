#include "musicserver.h"
MusicServer::MusicServer(QObject *parent)
    : QObject(parent),
      m_provider(m_settings.value("server/provider", "subsonic").toString()),
      m_sub(nullptr, m_provider != "jellyfin" && m_provider != "cider"),
      m_jelly(m_provider == "jellyfin"), m_cider(m_provider == "cider") {
  if (m_provider != "jellyfin" && m_provider != "cider")
    m_provider = "subsonic";
  for (auto *s : {&m_sub, static_cast<Subsonic *>(&m_jelly),
                  static_cast<Subsonic *>(&m_cider)}) {
    connect(s, &Subsonic::changed, this, &MusicServer::changed);
    connect(s, &Subsonic::accountChanged, this, &MusicServer::accountChanged);
    connect(s, &Subsonic::message, this, &MusicServer::message);
  }
}
void MusicServer::selectProvider(const QString &provider) {
  if (provider == m_provider ||
      !QStringList{"subsonic", "jellyfin", "cider"}.contains(provider))
    return;
  disconnectServer();
  m_provider = provider;
  m_settings.setValue("server/provider", provider);
  emit accountChanged();
  emit changed();
}
