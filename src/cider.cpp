#include "cider.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <memory>

namespace {
QString digest(const QString &s) {
  return QString::fromLatin1(
      QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString part(const QString &s) {
  return QString::fromLatin1(QUrl::toPercentEncoding(s));
}
QString credentialKey(const QString &address) {
  return "cider_" + digest(address);
}
// Apple's library identifiers carry a prefix per type: l. albums, p.
// playlists, r. artists and i. songs. Catalogue identifiers are numbers, and
// catalogue playlists start pl.
bool libraryId(const QString &id) {
  return id.size() > 2 && id[1] == '.' && QStringLiteral("lpri").contains(id[0]);
}
// The row list an Apple Music API answer carries, wherever it keeps it.
QVariantList resources(const QVariantMap &answer) {
  return answer.value("data").toList();
}
constexpr int maximumRows = 5000;
} // namespace

Cider::Cider(bool restore) : Subsonic(nullptr, false) {
  // Quitting Sung stops the song it started, as closing any player does.
  if (auto *app = QCoreApplication::instance())
    connect(app, &QCoreApplication::aboutToQuit, this, &Cider::shutDown);
  m_poll.setInterval(1000);
  connect(&m_poll, &QTimer::timeout, this, &Cider::poll);
  // Dragging the volume slider sends one request per settled value, not one
  // per pixel.
  m_volumeSend.setSingleShot(true);
  m_volumeSend.setInterval(120);
  connect(&m_volumeSend, &QTimer::timeout, this, [this] {
    if (m_connected && m_volume >= 0)
      send("POST", "/api/v1/playback/volume", {{"volume", m_volume}}, {});
  });
  if (!restore)
    return;
  m_address = m_settings.value("cider/address").toString();
  if (m_address.isEmpty())
    return;
  QTimer::singleShot(0, this, [this] {
    const auto address = m_address;
    auto reconnect = [this, address](const QString &token) {
      connectServer(address, {}, token, false);
    };
    if (!m_settings.value("cider/remember").toBool()) {
      if (m_settings.value("cider/tokenless").toBool())
        reconnect({});
      return;
    }
    const auto generation = m_generation;
    secret({"lookup", "application", "sung", "account", credentialKey(address)},
           {}, [this, generation, reconnect](bool ok, QByteArray token) {
             if (generation != m_generation)
               return;
             if (!ok || token.trimmed().isEmpty()) {
               m_error = "Enter the Cider token again to reconnect.";
               emit changed();
               return;
             }
             reconnect(QString::fromUtf8(token.trimmed()));
           });
  });
}
Cider::~Cider() { stopRequests(); }

void Cider::stopRequests() {
  ++m_generation;
  for (auto *r : m_network.findChildren<QNetworkReply *>()) {
    r->disconnect(this);
    r->abort();
    r->deleteLater();
  }
  m_channels.clear();
  m_pollBusy = false;
}
void Cider::cancel(const QString &channel) {
  auto r = m_channels.take(channel);
  if (r) {
    r->disconnect(this);
    r->abort();
    r->deleteLater();
  }
}

QNetworkRequest Cider::request(const QString &path) const {
  QNetworkRequest r(QUrl(m_address + path));
  // Playback calls go to this machine and answer at once; the Apple Music
  // pass-through waits on Apple's servers.
  r.setTransferTimeout(path.startsWith("/api/v1/amapi") ? 20000 : 4000);
  r.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                 QNetworkRequest::ManualRedirectPolicy);
  r.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
  // Cider reads the token bare from its own header, without "Bearer".
  if (!m_token.isEmpty())
    r.setRawHeader("apptoken", m_token.toUtf8());
  r.setRawHeader("Accept", "application/json");
  r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  return r;
}

void Cider::send(const QByteArray &method, const QString &path,
                 const QJsonObject &body, Reply callback,
                 const QString &channel) {
  if (!channel.isEmpty())
    cancel(channel);
  auto *r = m_network.sendCustomRequest(
      request(path), method,
      method == "GET" ? QByteArray()
                      : QJsonDocument(body).toJson(QJsonDocument::Compact));
  if (!channel.isEmpty())
    m_channels[channel] = r;
  ++m_inFlight;
  connect(r, &QObject::destroyed, this, [this] { --m_inFlight; });
  const auto generation = m_generation;
  auto oversized = std::make_shared<bool>(false);
  connect(r, &QNetworkReply::readyRead, this, [r, oversized] {
    if (r->bytesAvailable() > 16 * 1024 * 1024) {
      *oversized = true;
      r->abort();
    }
  });
  connect(r, &QNetworkReply::finished, this,
          [this, r, generation, callback, channel, oversized] {
            if (m_channels.value(channel) == r)
              m_channels.remove(channel);
            r->deleteLater();
            if (generation != m_generation || !callback)
              return;
            const int status =
                r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (*oversized) {
              callback({}, "Cider's answer was larger than 16 MiB.");
              return;
            }
            if (status == 401 || status == 403) {
              callback({{"httpStatus", status}},
                       "Cider did not accept the token. Make one in Cider "
                       "under Settings, Connectivity, Manage External "
                       "Application Access.");
              return;
            }
            if (status == 0 || r->error() == QNetworkReply::ConnectionRefusedError ||
                r->error() == QNetworkReply::HostNotFoundError) {
              callback({{"httpStatus", status}},
                       "Could not reach Cider. Open Cider and check that its "
                       "API is on in Settings, Connectivity.");
              return;
            }
            if (status < 200 || status >= 300) {
              callback({{"httpStatus", status}},
                       "Cider could not complete the request.");
              return;
            }
            const auto bytes = r->readAll();
            if (bytes.trimmed().isEmpty()) {
              callback({}, {});
              return;
            }
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError ||
                (!doc.isObject() && !doc.isArray())) {
              callback({}, "This address did not answer like Cider.");
              return;
            }
            callback(doc.isObject()
                         ? doc.object().toVariantMap()
                         : QVariantMap{{"items", doc.array().toVariantList()}},
                     {});
          });
}

// The pass-through wraps Apple's answer in a "data" object of its own.
void Cider::amapi(const QString &path, Reply callback, const QString &channel) {
  send(
      "POST", "/api/v1/amapi/run-v3", {{"path", path}},
      [callback](const QVariantMap &d, const QString &e) {
        if (!e.isEmpty()) {
          callback(d, d.value("httpStatus").toInt() == 0 ||
                              d.value("httpStatus").toInt() == 401 ||
                              d.value("httpStatus").toInt() == 403
                          ? e
                          : "Apple Music did not answer through Cider. Check "
                            "that Cider is signed in.");
          return;
        }
        const auto inner = d.value("data");
        callback(inner.typeId() == QMetaType::QVariantMap ? inner.toMap() : d,
                 {});
      },
      channel);
}

QString Cider::storefrontPath(const QString &rest) const {
  return "/v1/catalog/" + part(m_storefront) + rest;
}

void Cider::connectServer(const QString &address, const QString &,
                          const QString &token, bool remember) {
  QUrl base(address.trimmed().isEmpty() ? QStringLiteral("http://localhost:10767")
                                        : address.trimmed());
  if (!base.isValid() || !QStringList{"http", "https"}.contains(base.scheme()) ||
      base.host().isEmpty() || !base.userInfo().isEmpty() || base.hasQuery() ||
      base.hasFragment() || token.contains('\n') || token.contains('\r')) {
    m_error = "Enter Cider's address, such as http://localhost:10767.";
    emit changed();
    return;
  }
  auto path = base.path();
  while (path.endsWith('/'))
    path.chop(1);
  base.setPath(path);
  if (!remember && m_settings.value("cider/remember").toBool()) {
    secret({"clear", "application", "sung", "account",
            credentialKey(m_settings.value("cider/address").toString())},
           {}, [](bool, QByteArray) {});
    m_settings.setValue("cider/remember", false);
  }
  release();
  stopRequests();
  m_connected = false;
  m_connecting = true;
  m_error.clear();
  m_identity.clear();
  m_address = base.toString();
  m_token = token.trimmed();
  emit accountChanged();
  emit changed();
  const auto fail = [this](const QString &e) {
    m_connecting = false;
    m_error = e;
    m_token.clear();
    emit changed();
  };
  send(
      "GET", "/api/v1/playback/active", {},
      [this, fail, remember](const QVariantMap &, const QString &e) {
        if (!e.isEmpty()) {
          fail(e);
          return;
        }
        // The storefront is the listener's country, which every catalogue
        // path names.
        amapi(
            "/v1/me/storefront",
            [this, fail, remember](const QVariantMap &d, const QString &e) {
              if (!e.isEmpty()) {
                fail(e);
                return;
              }
              const auto front =
                  resources(d).value(0).toMap().value("id").toString();
              m_storefront = front.isEmpty() ? QStringLiteral("us") : front;
              m_connecting = false;
              m_connected = true;
              m_identity = digest("cider\n" + m_address);
              m_settings.setValue("cider/address", m_address);
              m_settings.setValue("cider/tokenless", m_token.isEmpty());
              if (remember && !m_token.isEmpty()) {
                const auto generation = m_generation;
                secret({"store", "--label=Sung Cider", "application", "sung",
                        "account", credentialKey(m_address)},
                       m_token.toUtf8(), [this, generation](bool ok, QByteArray) {
                         if (generation != m_generation)
                           return;
                         m_settings.setValue("cider/remember", ok);
                         if (!ok)
                           emit message("Connected for this session. The "
                                        "keyring could not save the token.");
                       });
              }
              emit changed();
            },
            "login");
      },
      "login");
}

void Cider::disconnectServer() {
  const bool remembered = m_settings.value("cider/remember").toBool();
  const auto key = credentialKey(m_address);
  // The pause and the autoplay setting are left to finish on their own; only
  // the answers are ignored from here.
  release();
  for (const auto &channel : m_channels.keys())
    cancel(channel);
  ++m_generation;
  m_pollBusy = false;
  m_token.clear();
  m_identity.clear();
  m_storefront.clear();
  m_connected = false;
  m_connecting = false;
  m_error.clear();
  m_address.clear();
  for (const auto &name : {"address", "remember", "tokenless"})
    m_settings.remove(QString("cider/") + name);
  if (remembered)
    secret({"clear", "application", "sung", "account", key}, {},
           [](bool, QByteArray) {});
  emit accountChanged();
  emit changed();
}

bool Cider::owns(const QVariantMap &track) const {
  return m_connected && track.value("source") == "cider" &&
         track.value("server") == m_identity;
}

QVariantMap Cider::item(const QVariantMap &resource, const QString &kind) const {
  const auto id = resource.value("id").toString();
  if (id.isEmpty())
    return {};
  const auto type = resource.value("type").toString();
  const auto a = resource.value("attributes").toMap();
  const auto play = a.value("playParams").toMap();
  // Apple's artwork addresses are templates: the size is filled in, and 600
  // matches what the other sources hand the interface.
  auto art = a.value("artwork").toMap().value("url").toString();
  art.replace("{w}", "600").replace("{h}", "600").replace("{c}", "bb").replace("{f}", "jpg");
  QVariantMap out{
      {"id", "am_" + digest(m_identity + "\n" + kind + "\n" + id)},
      {"source", "cider"},
      {"server", m_identity},
      {"remoteId", id},
      {"remoteType", type},
      {"kind", kind},
      {"title", a.value("name")},
      {"artist", a.value("artistName", a.value("curatorName"))},
      {"album", a.value("albumName")},
      {"seconds", a.value("durationInMillis").toLongLong() / 1000},
      {"discNumber", a.value("discNumber", 1)},
      {"trackNumber", a.value("trackNumber")},
      {"count", a.value("trackCount", 0)},
      // A library song Apple no longer carries has nothing to play.
      {"available", kind != "song" || !play.isEmpty()}};
  const auto year = a.value("releaseDate").toString().left(4).toInt();
  if (year > 0)
    out["year"] = year;
  if (art.startsWith("https://"))
    out["art"] = art;
  if (kind == "song") {
    out["serverSong"] = true;
    const auto catalog = play.value("catalogId").toString();
    if (!catalog.isEmpty())
      out["catalogId"] = catalog;
    else if (!libraryId(id))
      out["catalogId"] = id;
  }
  return out;
}

// Follows Apple's "next" links until `limit` rows or the end.
void Cider::collect(const QString &path, const QString &kind, int limit,
                    Reply callback, const QString &channel) {
  auto rows = std::make_shared<QVariantList>();
  auto step = std::make_shared<std::function<void(QString)>>();
  std::weak_ptr<std::function<void(QString)>> weak = step;
  *step = [this, kind, limit, callback, channel, rows, weak](const QString &at) {
    auto next = weak.lock();
    if (!next)
      return;
    amapi(
        at,
        [this, kind, limit, callback, rows, next](const QVariantMap &d,
                                                  const QString &e) {
          if (!e.isEmpty()) {
            callback({}, e);
            return;
          }
          for (const auto &v : resources(d)) {
            const auto row = item(v.toMap(), kind);
            if (!row.isEmpty())
              rows->append(row);
          }
          const auto more = d.value("next").toString();
          if (!more.startsWith("/v1/") || rows->size() >= limit) {
            callback({{"items", *rows}, {"more", more.startsWith("/v1/")}}, {});
            return;
          }
          (*next)(more);
        },
        channel);
  };
  // Each pending request holds the only strong reference to the next step,
  // so the chain lives exactly as long as it is fetching.
  (*step)(path);
}

void Cider::browse(const QVariantMap &req, Reply cb, const QString &channel) {
  if (!connected()) {
    cb({}, "Connect to Cider in Settings.");
    return;
  }
  const auto mode = req.value("mode", "albums").toString();
  const auto offset = QString::number(qMax(0, req.value("offset").toInt()));
  const auto remote = req.value("remoteId").toString();
  const auto id = part(remote);
  const bool library = libraryId(remote);
  auto page = [this, cb, channel](const QString &path, const QString &kind) {
    collect(path, kind, 100, cb, channel);
  };
  // A collection's own name and year come with it; its songs follow.
  auto detail = [this, cb, channel](const QString &meta, const QString &tracks) {
    amapi(
        meta,
        [this, cb, channel, tracks](const QVariantMap &d, const QString &e) {
          if (!e.isEmpty()) {
            cb({}, e);
            return;
          }
          const auto a = resources(d).value(0).toMap().value("attributes").toMap();
          collect(tracks, "song", maximumRows,
                  [cb, a](QVariantMap result, const QString &e) {
                    if (!e.isEmpty()) {
                      cb({}, e);
                      return;
                    }
                    result["more"] = false;
                    result["title"] = a.value("name");
                    result["artist"] = a.value("artistName", a.value("curatorName"));
                    const auto year = a.value("releaseDate").toString().left(4).toInt();
                    if (year > 0)
                      result["year"] = year;
                    cb(result, {});
                  },
                  channel);
        },
        channel);
  };
  if (mode == "albums")
    page("/v1/me/library/albums?limit=100&offset=" + offset, "album");
  else if (mode == "artists")
    page("/v1/me/library/artists?limit=100&offset=" + offset, "artist");
  else if (mode == "playlists")
    page("/v1/me/library/playlists?limit=100&offset=" + offset, "playlist");
  else if (mode == "album")
    detail(library ? "/v1/me/library/albums/" + id
                   : storefrontPath("/albums/" + id),
           library ? "/v1/me/library/albums/" + id + "/tracks?limit=100"
                   : storefrontPath("/albums/" + id + "/tracks?limit=100"));
  else if (mode == "playlist")
    detail(library ? "/v1/me/library/playlists/" + id
                   : storefrontPath("/playlists/" + id),
           library ? "/v1/me/library/playlists/" + id + "/tracks?limit=100"
                   : storefrontPath("/playlists/" + id + "/tracks?limit=100"));
  else if (mode == "artist")
    page((library ? "/v1/me/library/artists/" + id
                  : storefrontPath("/artists/" + id)) +
             "/albums?limit=100&offset=" + offset,
         "album");
  else if (mode == "search" || mode == "random") {
    // Discover shows the storefront's song chart, the nearest thing Apple
    // offers to Subsonic's random pick without a personal account history.
    const auto filter = req.value("filter", "songs").toString();
    const auto type = mode == "random" ? QStringLiteral("songs")
                      : filter == "albums"  ? QStringLiteral("albums")
                      : filter == "artists" ? QStringLiteral("artists")
                                            : QStringLiteral("songs");
    const auto kind = type.chopped(1);
    const auto path =
        mode == "random"
            ? storefrontPath("/charts?types=songs&limit=50")
            : storefrontPath("/search?term=" + part(req.value("query").toString()) +
                             "&types=" + type + "&limit=25&offset=" + offset);
    amapi(
        path,
        [this, cb, type, kind, mode](const QVariantMap &d, const QString &e) {
          if (!e.isEmpty()) {
            cb({}, e);
            return;
          }
          // Search answers under results.<type>; charts under a list of
          // charts per type.
          const auto found = d.value("results").toMap().value(type);
          const auto group = found.typeId() == QMetaType::QVariantList
                                 ? found.toList().value(0).toMap()
                                 : found.toMap();
          QVariantList rows;
          for (const auto &v : resources(group)) {
            const auto row = item(v.toMap(), kind);
            if (!row.isEmpty())
              rows << row;
          }
          cb({{"items", rows},
              {"more", mode == "search" && group.value("next").toString().startsWith("/v1/")}},
             {});
        },
        channel);
  } else
    cb({}, "Apple Music through Cider does not offer this view.");
}

void Cider::createPlaylist(const QString &) {
  emit message("Make Apple Music playlists in Cider; they appear here.");
}
void Cider::call(const QString &, const Params &, Reply cb, const QString &) {
  cb({}, "Apple Music through Cider does not support this.");
}
// Apple's lyrics need Apple's own player; the song's title and artist find
// them elsewhere instead.
void Cider::lyrics(const QVariantMap &, Reply cb) { cb({}, {}); }
void Cider::download(const QVariantMap &, const QString &, Reply cb) {
  cb({}, "Apple Music songs play in Cider and cannot be saved.");
}
void Cider::star(const QVariantMap &, bool, Reply cb) {
  cb({}, "Mark Apple Music favourites in Cider.");
}
void Cider::rate(const QVariantMap &, int, Reply cb) {
  cb({}, "Apple Music does not use five-star ratings.");
}
void Cider::editPlaylist(const QString &, const Params &, Reply cb) {
  cb({}, "Edit Apple Music playlists in Cider.");
}
void Cider::removePlaylist(const QString &, Reply cb) {
  cb({}, "Delete Apple Music playlists in Cider.");
}

// The desktop's media controls need the cover as a file.
void Cider::cover(const QVariantMap &track, const QString &path, Reply cb) {
  cancel("cover");
  const QUrl source(track.value("art").toString());
  if (source.scheme() != "https")
    return;
  QNetworkRequest r(source);
  r.setTransferTimeout(15000);
  auto *reply = m_network.get(r);
  m_channels["cover"] = reply;
  const auto generation = m_generation;
  connect(reply, &QNetworkReply::readyRead, this, [reply] {
    if (reply->bytesAvailable() > 4 * 1024 * 1024)
      reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation, path, cb] {
    if (m_channels.value("cover") == reply)
      m_channels.remove("cover");
    reply->deleteLater();
    if (generation != m_generation || reply->error() != QNetworkReply::NoError)
      return;
    const auto image = QImage::fromData(reply->readAll());
    if (image.isNull() || !image.save(path, "PNG"))
      return;
    cb({{"file", path}}, {});
  });
}

// Playback. Sung asks for one song, then watches Cider's clock once a second
// while it plays, and between those readings counts the time itself so the
// seek bar and the lyrics move smoothly without asking more often.

qint64 Cider::position() const {
  if (!m_playing)
    return m_base;
  const auto at = m_base + m_clock.elapsed();
  return m_duration > 0 ? qMin(at, m_duration) : at;
}

bool Cider::matches(const QVariantMap &info) const {
  const auto play = info.value("playParams").toMap();
  const auto id = play.value("id").toString(),
             catalog = play.value("catalogId").toString();
  if (!id.isEmpty() || !catalog.isEmpty())
    return (!id.isEmpty() && (id == m_song || id == m_catalogId)) ||
           (!catalog.isEmpty() && (catalog == m_song || catalog == m_catalogId));
  return !m_title.isEmpty() && info.value("name").toString() == m_title;
}

// Cider's own autoplay would start a song of its choosing when Sung's ends,
// and that belongs to Sung's queue. It is turned off while Sung drives and
// put back when Sung lets go; the flag outlives a crash so the next start
// can put it back too.
void Cider::holdAutoplay() {
  if (m_settings.value("cider/restoreAutoplay").toBool())
    return;
  send("GET", "/api/v1/playback/autoplay", {},
       [this](const QVariantMap &d, const QString &e) {
         if (!e.isEmpty() || !d.value("value").toBool() ||
             m_settings.value("cider/restoreAutoplay").toBool())
           return;
         m_settings.setValue("cider/restoreAutoplay", true);
         send("POST", "/api/v1/playback/toggle-autoplay", {}, {});
       });
}
void Cider::restoreAutoplay() {
  if (!m_settings.value("cider/restoreAutoplay").toBool())
    return;
  m_settings.remove("cider/restoreAutoplay");
  send("GET", "/api/v1/playback/autoplay", {},
       [this](const QVariantMap &d, const QString &e) {
         if (e.isEmpty() && !d.value("value").toBool())
           send("POST", "/api/v1/playback/toggle-autoplay", {}, {});
       });
}

void Cider::play(const QVariantMap &track, qint64 from) {
  if (!m_connected) {
    emit failed("Connect to Cider in Settings.");
    return;
  }
  m_song = track.value("remoteId").toString();
  m_catalogId = track.value("catalogId").toString();
  m_title = track.value("title").toString();
  m_duration = track.value("seconds").toLongLong() * 1000;
  m_base = qMax<qint64>(0, from);
  m_startAt = m_base;
  m_playing = false;
  m_starting = true;
  m_wanted = true;
  m_misses = 0;
  m_pollBusy = false;
  m_waited.restart();
  cancel("poll");
  holdAutoplay();
  if (m_volume >= 0)
    m_volumeSend.start();
  // The catalogue identifier plays for anyone subscribed; a song that exists
  // only in this library is named by its library address instead.
  const bool catalog = !m_catalogId.isEmpty();
  const auto song = m_song;
  send("POST",
       catalog ? "/api/v1/playback/play-item" : "/api/v1/playback/play-item-href",
       catalog ? QJsonObject{{"type", "songs"}, {"id", m_catalogId}}
               : QJsonObject{{"href", "/v1/me/library/songs/" + m_song}},
       [this, song](const QVariantMap &, const QString &e) {
         if (song != m_song)
           return;
         if (!e.isEmpty()) {
           abandon(e);
           return;
         }
         m_poll.start();
         QTimer::singleShot(300, this, &Cider::poll);
       },
       "transport");
  emit transportChanged();
}

void Cider::poll() {
  if (!active() || !m_wanted || m_pollBusy)
    return;
  m_pollBusy = true;
  const auto song = m_song;
  const auto generation = m_generation;
  auto missed = [this](const QString &e) {
    m_pollBusy = false;
    if (++m_misses < 3)
      return;
    m_base = position();
    abandon(e);
  };
  send(
      "GET", "/api/v1/playback/now-playing", {},
      [this, song, generation, missed](const QVariantMap &d, const QString &e) {
        if (song != m_song || generation != m_generation) {
          m_pollBusy = false;
          return;
        }
        // Nothing loaded answers 404 or an empty body.
        const int status = d.value("httpStatus").toInt();
        if (!e.isEmpty() && status != 404 && status != 204) {
          missed(e);
          return;
        }
        const auto info = e.isEmpty() ? d.value("info").toMap() : QVariantMap{};
        send("GET", "/api/v1/playback/is-playing", {},
             [this, song, info, missed](const QVariantMap &p, const QString &e) {
               if (song != m_song) {
                 m_pollBusy = false;
                 return;
               }
               if (!e.isEmpty()) {
                 missed(e);
                 return;
               }
               m_pollBusy = false;
               m_misses = 0;
               observe(info, p.value("is_playing").toBool());
             },
             "poll");
      },
      "poll");
}

void Cider::settle() {
  m_starting = m_playing = m_wanted = false;
  m_poll.stop();
  emit transportChanged();
}
// Lets go without pausing: Cider is either unreachable or doing something
// the listener chose there. The position read last stays for a retry.
void Cider::abandon(const QString &message) {
  m_starting = m_playing = m_wanted = false;
  m_poll.stop();
  cancel("poll");
  m_pollBusy = false;
  m_song.clear();
  m_catalogId.clear();
  m_title.clear();
  if (m_connected)
    restoreAutoplay();
  emit transportChanged();
  emit failed(message);
}

void Cider::observe(const QVariantMap &info, bool playing) {
  const bool ours = !info.isEmpty() && matches(info);
  const auto at = qint64(info.value("currentPlaybackTime").toDouble() * 1000);
  const auto total = info.value("durationInMillis").toLongLong();
  if (m_starting) {
    if (ours && playing) {
      m_starting = false;
      m_playing = true;
      if (total > 0)
        m_duration = total;
      m_base = at;
      m_clock.restart();
      // A song resumed part way starts from the top in Cider, then moves.
      if (m_startAt > 1000 && at < m_startAt - 1500)
        seek(m_startAt);
      m_startAt = 0;
      emit transportChanged();
    } else if (m_waited.elapsed() > 20000) {
      abandon("Cider did not start this song. Check that Apple Music is "
              "signed in within Cider.");
    }
    return;
  }
  // Seen from the last reading, the song was within a few seconds of its end.
  const auto last = position();
  const bool nearEnd = m_duration > 0 && m_duration - last <= 4000;
  if (!ours) {
    if (m_playing && nearEnd) {
      m_base = m_duration;
      settle();
      emit finished();
    } else {
      // Someone chose another song in Cider itself. Sung stops following
      // rather than fighting over it.
      m_base = last;
      abandon("Cider is playing something else now.");
    }
    return;
  }
  if (total > 0)
    m_duration = total;
  if (playing) {
    m_base = at;
    m_clock.restart();
    if (!m_playing) {
      m_playing = true;
      emit transportChanged();
    }
    emit progressed();
    // The end is read as soon as it is due rather than up to a second late.
    const auto left = m_duration - at;
    if (left > 0 && left < 1500)
      QTimer::singleShot(left + 150, this, &Cider::poll);
    return;
  }
  if (m_playing && (nearEnd || (m_duration > 0 && m_duration - at <= 1500))) {
    m_base = m_duration;
    settle();
    emit finished();
    return;
  }
  // Paused from Cider's own window or its media keys.
  m_base = at;
  settle();
}

void Cider::pause() {
  m_wanted = false;
  if (!active())
    return;
  m_base = position();
  m_playing = m_starting = false;
  m_poll.stop();
  cancel("poll");
  m_pollBusy = false;
  send("POST", "/api/v1/playback/pause", {}, {});
  emit transportChanged();
}

void Cider::resume() {
  if (!active())
    return;
  m_wanted = true;
  m_playing = true;
  m_clock.restart();
  send("POST", "/api/v1/playback/play", {}, {});
  m_poll.start();
  emit transportChanged();
}

void Cider::seek(qint64 milliseconds) {
  if (!active())
    return;
  const auto target =
      qBound<qint64>(0, milliseconds, m_duration > 0 ? m_duration : milliseconds);
  m_base = target;
  m_clock.restart();
  if (m_starting)
    m_startAt = target;
  send("POST", "/api/v1/playback/seek", {{"position", double(target) / 1000.0}}, {});
  emit transportChanged();
}

void Cider::setVolume(double volume) {
  m_volume = qBound(0.0, volume, 1.0);
  if (active())
    m_volumeSend.start();
}

void Cider::release() {
  if (!active())
    return;
  if ((m_playing || m_starting) && m_connected)
    send("POST", "/api/v1/playback/pause", {}, {});
  m_song.clear();
  m_catalogId.clear();
  m_title.clear();
  m_playing = m_starting = m_wanted = false;
  m_base = m_duration = 0;
  m_poll.stop();
  cancel("poll");
  cancel("transport");
  m_pollBusy = false;
  if (m_connected)
    restoreAutoplay();
  emit transportChanged();
}

void Cider::shutDown() {
  if (!active() || !m_connected)
    return;
  release();
  // The pause, and putting autoplay back, which asks first and then sets.
  QEventLoop loop;
  QTimer limit, check;
  limit.setSingleShot(true);
  connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
  check.setInterval(10);
  connect(&check, &QTimer::timeout, &loop, [this, &loop] {
    if (m_inFlight <= 0)
      loop.quit();
  });
  limit.start(800);
  check.start();
  loop.exec();
}
