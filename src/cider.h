#pragma once
#include "subsonic.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>

// Apple Music through Cider, the Apple Music client from cider.sh. Cider holds
// the Apple Music session and plays the audio, which Apple only streams to
// its own players; it serves a REST API on the listener's machine, port 10767
// unless changed, authorised by a token made in Cider under Settings,
// Connectivity, Manage External Application Access. The API is documented at
// https://cider.gitbook.io/welcome-to-gitbook/docs/1.client/rpc
//
// Sung browses the catalogue and the library through the API's Apple Music
// pass-through (amapi/run-v3) and keeps its own queue. It hands Cider one song
// at a time and moves on when that song ends, so the queue, history, lyrics
// and the whole interface stay Sung's while the sound comes out of Cider.
class Cider : public Subsonic {
  Q_OBJECT
public:
  explicit Cider(bool restore = true);
  ~Cider() override;
  bool connected() const { return m_connected; }
  bool connecting() const { return m_connecting; }
  QString address() const { return m_address; }
  QString username() const { return {}; }
  QString identity() const { return m_identity; }
  QString error() const { return m_error; }
  QVariantList playlists() const { return {}; }
  QVariantList folders() const { return {}; }
  QString folder() const { return {}; }
  bool scrobbling() const { return false; }
  int bitrate() const { return 0; }
  void setFolder(const QString &) {}
  void setScrobbling(bool) {}
  void setBitrate(int) {}
  // The token may be empty: Cider can be set to take requests without one.
  void connectServer(const QString &address, const QString &user,
                     const QString &token, bool remember = true);
  void disconnectServer();
  void reloadPlaylists() {}
  void createPlaylist(const QString &);
  void call(const QString &, const Params &, Reply callback,
            const QString &channel = {});
  void cancel(const QString &channel);
  void browse(const QVariantMap &request, Reply callback,
              const QString &channel = "catalog");
  void cover(const QVariantMap &, const QString &, Reply callback);
  void lyrics(const QVariantMap &, Reply callback);
  void download(const QVariantMap &, const QString &, Reply callback);
  void star(const QVariantMap &, bool, Reply callback);
  void rate(const QVariantMap &, int, Reply callback);
  void editPlaylist(const QString &, const Params &, Reply callback);
  void removePlaylist(const QString &, Reply callback);
  void scrobble(const QVariantMap &, bool, qint64, Reply callback) {
    callback({}, {});
  }
  bool owns(const QVariantMap &track) const;
  bool isStarred(const QString &) const { return false; }
  QVariantMap item(const QVariantMap &resource, const QString &kind) const;
  QUrl artworkUrl(const QUrl &) const { return {}; }

  // Playback, which happens in Cider. Sung's own decks stay silent while a
  // song from here is current.
  void play(const QVariantMap &track, qint64 from);
  void pause();
  void resume();
  void seek(qint64 milliseconds);
  void setVolume(double volume);
  // Stops following Cider, pausing it if it was playing for Sung.
  void release();
  // Run as the application quits: lets go, and waits up to 800 ms for Cider
  // to hear it, since nothing is sent once the event loop has stopped.
  void shutDown();
  bool active() const { return !m_song.isEmpty(); }
  bool playing() const { return m_playing; }
  bool starting() const { return m_starting; }
  qint64 position() const;
  qint64 duration() const { return m_duration; }

signals:
  void transportChanged();
  // The song Sung asked for has played to its end.
  void finished();
  // Each reading of Cider's clock while the song plays.
  void progressed();
  // Sung has stopped following Cider; the reason is for the listener.
  void failed(const QString &message);

private:
  QNetworkRequest request(const QString &path) const;
  void send(const QByteArray &method, const QString &path,
            const QJsonObject &body, Reply callback,
            const QString &channel = {});
  void amapi(const QString &path, Reply callback, const QString &channel = {});
  void collect(const QString &path, const QString &kind, int limit,
               Reply callback, const QString &channel);
  QString storefrontPath(const QString &rest) const;
  void poll();
  void observe(const QVariantMap &info, bool playing);
  void settle();
  void abandon(const QString &message);
  void holdAutoplay();
  void restoreAutoplay();
  void stopRequests();
  bool matches(const QVariantMap &info) const;

  QNetworkAccessManager m_network;
  QSettings m_settings;
  QString m_address, m_token, m_identity, m_error, m_storefront;
  bool m_connected = false, m_connecting = false;
  quint64 m_generation = 0;
  int m_inFlight = 0;
  QHash<QString, QPointer<QNetworkReply>> m_channels;

  QTimer m_poll, m_volumeSend;
  QElapsedTimer m_clock, m_waited;
  QString m_song, m_catalogId, m_title;
  bool m_playing = false, m_starting = false, m_wanted = false,
       m_pollBusy = false;
  qint64 m_base = 0, m_duration = 0, m_startAt = 0;
  int m_misses = 0;
  double m_volume = -1;
};
