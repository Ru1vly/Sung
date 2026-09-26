#pragma once
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QProcess>
#include <QTimer>

// What the speakers are playing, read back from the default output's monitor
// with parec. A song from Cider is decoded and played by Cider, so Sung's
// own audio tap has nothing to measure; this gives the meters, the play
// button's pulse and the visualizer the same sound the listener hears.
//
// parec ships in the same package as the pactl Sung already uses for output
// devices (libpulse on Arch and CachyOS), and PipeWire answers it through
// pipewire-pulse. It runs only while something on screen reads the levels,
// and desktops may show it as an application recording audio while it does.
class OutputTap : public QObject {
  Q_OBJECT
public:
  explicit OutputTap(QObject *parent = nullptr);
  ~OutputTap() override;
  void setRunning(bool running);
  bool running() const { return m_process.state() != QProcess::NotRunning; }
signals:
  void buffer(const QAudioBuffer &buffer);

private:
  void start();
  QProcess m_process;
  QTimer m_retry;
  QAudioFormat m_format;
  QByteArray m_pending;
  qint64 m_frames = 0;
  bool m_wanted = false;
  int m_failures = 0;
};
