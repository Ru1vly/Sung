#pragma once
#include "cider.h"
#include "jellyfin.h"
#include "subsonic.h"
inline bool isServerSource(const QVariant &source) {
  return source == "subsonic" || source == "jellyfin" || source == "cider";
}
class MusicServer : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY changed)
  Q_PROPERTY(bool connecting READ connecting NOTIFY changed)
  Q_PROPERTY(QString address READ address NOTIFY changed)
  Q_PROPERTY(QString username READ username NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString identity READ identity NOTIFY changed)
  Q_PROPERTY(QVariantList playlists READ playlists NOTIFY changed)
  Q_PROPERTY(QVariantList folders READ folders NOTIFY changed)
  Q_PROPERTY(QString folder READ folder WRITE setFolder NOTIFY changed)
  Q_PROPERTY(bool scrobbling READ scrobbling WRITE setScrobbling NOTIFY changed)
  Q_PROPERTY(int bitrate READ bitrate WRITE setBitrate NOTIFY changed)
  Q_PROPERTY(bool keyringAvailable READ keyringAvailable CONSTANT)
  Q_PROPERTY(QString provider READ provider NOTIFY changed)
  Q_PROPERTY(bool supportsRating READ supportsRating NOTIFY changed)
  Q_PROPERTY(bool supportsQueue READ supportsQueue NOTIFY changed)
  // The library views this provider can list, as the page's chips name them.
  Q_PROPERTY(QStringList modes READ modes NOTIFY changed)
  // Songs from this provider play in another application, not in Sung.
  Q_PROPERTY(bool remotePlayback READ remotePlayback NOTIFY changed)
  // Jellyfin hides Subsonic's members rather than overriding them, so the
  // call has to reach the concrete type: a generic lambda gets it.
  template <class Call> decltype(auto) with(Call call) const {
    if (m_provider == "jellyfin")
      return call(m_jelly);
    if (m_provider == "cider")
      return call(m_cider);
    return call(m_sub);
  }
  template <class Call> decltype(auto) with(Call call) {
    if (m_provider == "jellyfin")
      return call(m_jelly);
    if (m_provider == "cider")
      return call(m_cider);
    return call(m_sub);
  }

public:
  using Reply = Subsonic::Reply;
  using Params = Subsonic::Params;
  explicit MusicServer(QObject *parent = nullptr);
  QString provider() const { return m_provider; }
  bool supportsRating() const { return m_provider == "subsonic"; }
  bool supportsQueue() const { return m_provider == "subsonic"; }
  QStringList modes() const {
    if (m_provider == "cider")
      return {"albums", "artists", "playlists", "random"};
    return {"albums", "artists", "genres", "playlists", "favorites", "random"};
  }
  bool remotePlayback() const { return m_provider == "cider"; }
  Cider *cider() { return &m_cider; }
  const Cider *cider() const { return &m_cider; }
  Q_INVOKABLE void selectProvider(const QString &provider);
  bool connected() const { return with([](auto &s) { return s.connected(); }); }
  bool connecting() const { return with([](auto &s) { return s.connecting(); }); }
  QString address() const { return with([](auto &s) { return s.address(); }); }
  QString username() const { return with([](auto &s) { return s.username(); }); }
  QString identity() const { return with([](auto &s) { return s.identity(); }); }
  QString error() const { return with([](auto &s) { return s.error(); }); }
  QVariantList playlists() const { return with([](auto &s) { return s.playlists(); }); }
  QVariantList folders() const { return with([](auto &s) { return s.folders(); }); }
  QString folder() const { return with([](auto &s) { return s.folder(); }); }
  bool scrobbling() const { return with([](auto &s) { return s.scrobbling(); }); }
  int bitrate() const { return with([](auto &s) { return s.bitrate(); }); }
  bool keyringAvailable() const {
    return with([](auto &s) { return s.keyringAvailable(); });
  }
  void setFolder(const QString &id) { with([&](auto &s) { s.setFolder(id); }); }
  void setScrobbling(bool value) { with([&](auto &s) { s.setScrobbling(value); }); }
  void setBitrate(int value) { with([&](auto &s) { s.setBitrate(value); }); }
  Q_INVOKABLE void connectServer(const QString &address, const QString &user,
                                 const QString &password,
                                 bool remember = true) {
    with([&](auto &s) { s.connectServer(address, user, password, remember); });
  }
  Q_INVOKABLE void disconnectServer() { with([](auto &s) { s.disconnectServer(); }); }
  Q_INVOKABLE void reloadPlaylists() { with([](auto &s) { s.reloadPlaylists(); }); }
  Q_INVOKABLE void createPlaylist(const QString &name) {
    with([&](auto &s) { s.createPlaylist(name); });
  }
  void call(const QString &method, const Params &params, Reply callback,
            const QString &channel = {}) {
    with([&](auto &s) { s.call(method, params, callback, channel); });
  }
  void cancel(const QString &channel) { with([&](auto &s) { s.cancel(channel); }); }
  void browse(const QVariantMap &request, Reply callback,
              const QString &channel = "catalog") {
    with([&](auto &s) { s.browse(request, callback, channel); });
  }
  void cover(const QVariantMap &track, const QString &path, Reply callback) {
    with([&](auto &s) { s.cover(track, path, callback); });
  }
  void lyrics(const QVariantMap &track, Reply callback) {
    with([&](auto &s) { s.lyrics(track, callback); });
  }
  void download(const QVariantMap &track, const QString &path, Reply callback) {
    with([&](auto &s) { s.download(track, path, callback); });
  }
  void star(const QVariantMap &track, bool value, Reply callback) {
    with([&](auto &s) { s.star(track, value, callback); });
  }
  void rate(const QVariantMap &track, int value, Reply callback) {
    with([&](auto &s) { s.rate(track, value, callback); });
  }
  void editPlaylist(const QString &id, const Params &params, Reply callback) {
    with([&](auto &s) { s.editPlaylist(id, params, callback); });
  }
  void removePlaylist(const QString &id, Reply callback) {
    with([&](auto &s) { s.removePlaylist(id, callback); });
  }
  void scrobble(const QVariantMap &track, bool submission, qint64 timestamp,
                Reply callback) {
    with([&](auto &s) { s.scrobble(track, submission, timestamp, callback); });
  }
  bool owns(const QVariantMap &track) const {
    return with([&](auto &s) { return s.owns(track); });
  }
  bool isStarred(const QString &id) const {
    return with([&](auto &s) { return s.isStarred(id); });
  }
  QVariantMap item(const QVariantMap &raw, const QString &kind) const {
    return with([&](auto &s) { return s.item(raw, kind); });
  }
  QUrl artworkUrl(const QUrl &reference) const {
    return with([&](auto &s) { return s.artworkUrl(reference); });
  }
  QNetworkRequest artworkRequest(const QUrl &reference) const {
    return m_provider == "jellyfin"
               ? m_jelly.artworkRequest(reference)
               : QNetworkRequest(m_sub.artworkUrl(reference));
  }
  void reportPlayback(const QVariantMap &track, qint64 position, bool paused,
                      bool stopped) {
    if (m_provider == "jellyfin")
      m_jelly.reportPlayback(track, position, paused, stopped);
  }
signals:
  void changed();
  void accountChanged();
  void message(const QString &text);

private:
  QSettings m_settings;
  QString m_provider;
  Subsonic m_sub;
  Jellyfin m_jelly;
  Cider m_cider;
};
