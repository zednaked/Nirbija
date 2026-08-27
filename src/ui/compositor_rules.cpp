#include "compositor_rules.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QString>

namespace nirbija {
namespace {

// qWarning rather than qDebug for the same reason plugin_window.cpp uses it:
// a release build compiles qDebug away, and this trace is wanted exactly when
// someone is chasing an editor window on a machine running the release build.
bool tracing() { return !qEnvironmentVariableIsEmpty("NIRBIJA_DEBUG_EMBED"); }

// Hyprland listens on a unix socket per running instance. The signature names
// the instance; its absence is the check for "not running under Hyprland",
// since the compositor exports it into every client's environment.
QString socket_path() {
  const QByteArray signature = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (signature.isEmpty()) return {};

  // Since 0.40 the socket lives under the runtime dir. Older builds put it in
  // /tmp, and a machine can still be running one of those.
  const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
  if (!runtime.isEmpty()) {
    const QString path = QString::fromLocal8Bit(runtime) + "/hypr/" +
                         QString::fromLocal8Bit(signature) + "/.socket.sock";
    if (QFile::exists(path)) return path;
  }
  const QString legacy =
      "/tmp/hypr/" + QString::fromLocal8Bit(signature) + "/.socket.sock";
  if (QFile::exists(legacy)) return legacy;
  return {};
}

// One request, one reply, then the socket closes - that is the whole protocol.
// Written with plain sockets rather than QLocalSocket so this works with no
// event loop and no application object around it.
QByteArray ask(const QString& path, const QByteArray& request) {
  const QByteArray native = path.toLocal8Bit();
  if (native.size() >= static_cast<int>(sizeof(sockaddr_un::sun_path))) return {};

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return {};

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, native.constData(), static_cast<size_t>(native.size()));

  QByteArray reply;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
      ::write(fd, request.constData(), static_cast<size_t>(request.size())) ==
          request.size()) {
    char buffer[512];
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got > 0) reply = QByteArray(buffer, static_cast<int>(got));
  }
  ::close(fd);
  return reply;
}

}  // namespace

void apply_compositor_rules() {
  if (!qEnvironmentVariableIsEmpty("NIRBIJA_NO_WM_RULES")) return;

  const QString path = socket_path();
  if (path.isEmpty()) return;  // not Hyprland, or its socket is gone

  // Hyprland 0.56 moved the config to Lua and its `keyword` command now refuses
  // anything the legacy parser cannot take - a windowrule included, answered
  // with "keyword can't work with non-legacy parsers. Use eval." So the rule
  // goes in as Lua first, and only older builds fall through to `keyword`.
  //
  // The rule is named so that re-running it replaces the previous one rather
  // than stacking a second copy: this runs on every launch, and a second mixer
  // window should not leave the compositor holding two identical rules.
  static const QByteArray lua =
      "/eval hl.window_rule({ name = \"nirbija_plugin_editor\", "
      "match = { class = \"^(nirbija-plugin)$\" }, float = true, center = true })";
  QByteArray reply = ask(path, lua);
  if (reply.startsWith("ok")) {
    if (tracing()) qWarning("wmrule: applied via hyprland lua");
    return;
  }
  if (tracing()) qWarning("wmrule: hyprland refused the lua form: %s", reply.constData());

  // Pre-Lua Hyprland: no `eval`, but `keyword` still takes a rule.
  static const QByteArray legacy =
      "/keyword windowrulev2 float,class:^(nirbija-plugin)$";
  reply = ask(path, legacy);
  if (tracing()) {
    if (reply.startsWith("ok"))
      qWarning("wmrule: applied via hyprland keyword");
    else
      qWarning("wmrule: no rule applied: %s", reply.constData());
  }
}

}  // namespace nirbija
