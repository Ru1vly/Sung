#include "outputtap.h"

namespace {
// 48 kHz stereo float, the format the analysers already take from Sung's own
// decks, in blocks of 1024 frames: about 21 ms, close to what a deck hands
// its buffer output.
constexpr int rate = 48000, channels = 2, block = 1024;
} // namespace

OutputTap::OutputTap(QObject *parent) : QObject(parent) {
  m_format.setSampleRate(rate);
  m_format.setChannelCount(channels);
  m_format.setSampleFormat(QAudioFormat::Float);
  m_retry.setSingleShot(true);
  m_retry.setInterval(2000);
  connect(&m_retry, &QTimer::timeout, this, [this] {
    if (m_wanted && !running())
      start();
  });
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    m_pending += m_process.readAllStandardOutput();
    const qsizetype bytes = qsizetype(block) * channels * sizeof(float);
    while (m_pending.size() >= bytes) {
      const QAudioBuffer chunk(m_pending.left(bytes), m_format,
                               m_frames * 1000000 / rate);
      m_pending.remove(0, bytes);
      m_frames += block;
      m_failures = 0;
      emit buffer(chunk);
    }
  });
  // A sound server that restarts takes the recording with it; a missing
  // parec never starts. Either is tried again a few times, then left alone.
  connect(&m_process, &QProcess::finished, this, [this] {
    if (m_wanted && ++m_failures <= 3)
      m_retry.start();
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart && m_wanted && ++m_failures <= 3)
      m_retry.start();
  });
}

OutputTap::~OutputTap() {
  m_wanted = false;
  m_process.kill();
  m_process.waitForFinished(500);
}

void OutputTap::start() {
  m_pending.clear();
  // SUNG_OUTPUT_TAP stands in for parec in the tests, where no speaker plays.
  const auto program = qEnvironmentVariable("SUNG_OUTPUT_TAP", "parec");
  m_process.start(program, {"--device=@DEFAULT_MONITOR@", "--raw", "--format=float32le",
                            "--rate=" + QString::number(rate),
                            "--channels=" + QString::number(channels),
                            "--latency-msec=30", "--client-name=Sung",
                            "--stream-name=Visualizer"});
}

void OutputTap::setRunning(bool running) {
  if (m_wanted == running)
    return;
  m_wanted = running;
  m_retry.stop();
  if (running) {
    m_failures = 0;
    if (!this->running())
      start();
    return;
  }
  m_process.kill();
  m_process.waitForFinished(200);
  m_pending.clear();
}
