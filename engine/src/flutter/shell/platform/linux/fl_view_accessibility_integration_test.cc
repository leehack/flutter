// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <gtk/gtk.h>
#include <unistd.h>

#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "gtest/gtest.h"

namespace {

// This target deliberately excludes LinuxTest and GTK/ATK mocks. Run in a fresh
// dbus-run-session and display with GTK_MODULES=gail:atk-bridge NO_AT_BRIDGE=0.
// An outer `timeout 45s` contains the old synchronous bridge self-call.
struct AccessibilityBox {
  GtkBox parent_instance;
};
struct AccessibilityBoxClass {
  GtkBoxClass parent_class;
};
G_DEFINE_TYPE(AccessibilityBox, accessibility_box, GTK_TYPE_BOX)
static void accessibility_box_class_init(AccessibilityBoxClass* klass) {
  gtk_widget_class_set_accessible_type(GTK_WIDGET_CLASS(klass),
                                       fl_view_accessible_get_type());
}
static void accessibility_box_init(AccessibilityBox*) {}

static GVariant* call(GDBusConnection* bus,
                      const gchar* destination,
                      const gchar* path,
                      const gchar* interface,
                      const gchar* method,
                      GVariant* args = nullptr) {
  g_autoptr(GError) error = nullptr;
  GVariant* result = g_dbus_connection_call_sync(
      bus, destination, path, interface, method, args, nullptr,
      G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
  EXPECT_EQ(error, nullptr) << (error != nullptr ? error->message : "");
  return result;
}

static gchar* find_by_name(GDBusConnection* bus,
                           const gchar* destination,
                           const gchar* path,
                           const gchar* name,
                           guint depth = 0) {
  if (depth > 8) {
    return nullptr;
  }
  g_autoptr(GVariant) reply =
      call(bus, destination, path, "org.freedesktop.DBus.Properties", "Get",
           g_variant_new("(ss)", "org.a11y.atspi.Accessible", "Name"));
  if (reply == nullptr) {
    return nullptr;
  }
  g_autoptr(GVariant) value = nullptr;
  g_variant_get(reply, "(v)", &value);
  if (g_strcmp0(g_variant_get_string(value, nullptr), name) == 0) {
    return g_strdup(path);
  }
  g_autoptr(GVariant) children =
      call(bus, destination, path, "org.a11y.atspi.Accessible", "GetChildren");
  if (children == nullptr) {
    return nullptr;
  }
  g_autoptr(GVariantIter) iter = nullptr;
  g_variant_get(children, "(a(so))", &iter);
  const gchar *child_bus, *child_path;
  while (g_variant_iter_next(iter, "(&s&o)", &child_bus, &child_path)) {
    if (g_strcmp0(child_bus, destination) != 0) {
      continue;
    }
    gchar* result = find_by_name(bus, destination, child_path, name, depth + 1);
    if (result != nullptr) {
      return result;
    }
  }
  return nullptr;
}

class FlViewAccessibilityIntegrationTest : public ::testing::Test {};

// The helper is a newly exec'd process, never a forked copy of GTK/bridge
// state.
TEST_F(FlViewAccessibilityIntegrationTest, ExternalClient) {
  const gchar* address = g_getenv("FL_A11Y_TEST_ADDRESS");
  if (address == nullptr) {
    GTEST_SKIP() << "Only invoked by MappedGeometryAndDispatch";
  }
  g_autoptr(GError) error = nullptr;
  g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  ASSERT_NE(bus, nullptr) << (error != nullptr ? error->message : "");
  const gchar* destination = g_getenv("FL_A11Y_TEST_BUS");
  const gchar* root = "/org/a11y/atspi/accessible/root";
  if (g_strcmp0(g_getenv("FL_A11Y_TEST_METHOD"), "role") == 0) {
    g_usleep(100 * 1000);
    g_autoptr(GVariant) reply =
        call(bus, destination, root, "org.a11y.atspi.Accessible", "GetRole");
    ASSERT_NE(reply, nullptr);
    return;
  }
  g_autofree gchar* path = find_by_name(bus, destination, root, "a11y-view");
  ASSERT_NE(path, nullptr);
  gint expected[4];
  ASSERT_EQ(sscanf(g_getenv("FL_A11Y_TEST_RECT"), "%d,%d,%d,%d", &expected[0],
                   &expected[1], &expected[2], &expected[3]),
            4);
  gint64 started = g_get_monotonic_time();
  g_autoptr(GVariant) reply =
      call(bus, destination, path, "org.a11y.atspi.Component", "GetExtents",
           g_variant_new("(u)", 0));
  ASSERT_NE(reply, nullptr);
  EXPECT_LT(g_get_monotonic_time() - started, G_TIME_SPAN_SECOND);
  gint x, y, width, height;
  g_variant_get(reply, "((iiii))", &x, &y, &width, &height);
  EXPECT_EQ(x, expected[0]);
  EXPECT_EQ(y, expected[1]);
  EXPECT_EQ(width, expected[2]);
  EXPECT_EQ(height, expected[3]);
}

struct ClientResult {
  gboolean done = FALSE;
  gboolean success = FALSE;
};

static void client_done(GObject* source, GAsyncResult* result, gpointer data) {
  ClientResult* observation = static_cast<ClientResult*>(data);
  g_autoptr(GError) error = nullptr;
  observation->success =
      g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
  observation->done = TRUE;
}

struct Heartbeat {
  gint64 last = g_get_monotonic_time();
  gint64 longest = 0;
  guint ticks = 0;
};

TEST_F(FlViewAccessibilityIntegrationTest, MappedGeometryAndDispatch) {
  ASSERT_TRUE(gtk_init_check(nullptr, nullptr));
  g_autoptr(GDBusConnection) session =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  ASSERT_NE(session, nullptr);
  g_autoptr(GVariant) address_reply = call(
      session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
  ASSERT_NE(address_reply, nullptr);
  const gchar* address;
  g_variant_get(address_reply, "(&s)", &address);
  g_autoptr(GDBusConnection) bus = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, nullptr);
  ASSERT_NE(bus, nullptr);
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_object_ref_sink(window);
  GtkWidget* fixed = gtk_fixed_new();
  gtk_container_add(GTK_CONTAINER(window), fixed);
  GtkWidget* view =
      GTK_WIDGET(g_object_new(accessibility_box_get_type(), nullptr));
  gtk_widget_set_size_request(view, 280, 180);
  gtk_fixed_put(GTK_FIXED(fixed), view, 31, 47);
  g_autoptr(FlViewAccessible) accessible =
      FL_VIEW_ACCESSIBLE(g_object_ref(gtk_widget_get_accessible(view)));
  fl_view_accessible_set_engine(accessible, engine, 456);
  atk_object_set_name(ATK_OBJECT(accessible), "a11y-view");
  FlutterSemanticsFlags flags = {};
  FlutterSemanticsNode2 node = {};
  node.id = 0;
  node.label = "a11y-semantic-root";
  node.flags2 = &flags;
  FlutterSemanticsNode2* nodes[] = {&node};
  FlutterSemanticsUpdate2 update = {};
  update.node_count = 1;
  update.nodes = nodes;
  fl_view_accessible_handle_update_semantics(accessible, &update);
  gtk_widget_show_all(window);

  // The real bridge must register this process. Module flags alone are not
  // readiness; each external client additionally discovers the exported tree.
  g_autofree gchar* destination = nullptr;
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (destination == nullptr && g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    g_autoptr(GVariant) names =
        call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
             "org.freedesktop.DBus", "ListNames");
    ASSERT_NE(names, nullptr);
    g_auto(GStrv) values = nullptr;
    g_variant_get(names, "(^as)", &values);
    for (guint i = 0; values[i] != nullptr; i++) {
      if (values[i][0] != ':' ||
          g_strcmp0(values[i], g_dbus_connection_get_unique_name(bus)) == 0)
        continue;
      g_autoptr(GVariant) pid_reply =
          call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
               "org.freedesktop.DBus", "GetConnectionUnixProcessID",
               g_variant_new("(s)", values[i]));
      if (pid_reply == nullptr)
        continue;
      guint pid;
      g_variant_get(pid_reply, "(u)", &pid);
      if (pid == static_cast<guint>(getpid()))
        destination = g_strdup(values[i]);
    }
    g_usleep(1000);
  }
  ASSERT_NE(destination, nullptr);
  ASSERT_TRUE(gtk_widget_get_mapped(view));
  gint x, y, sx, sy;
  ASSERT_TRUE(gtk_widget_translate_coordinates(view, window, 0, 0, &x, &y));
  gdk_window_get_origin(gtk_widget_get_window(window), &sx, &sy);
  GtkAllocation allocation;
  gtk_widget_get_allocation(view, &allocation);
  g_autofree gchar* rectangle = g_strdup_printf(
      "%d,%d,%d,%d", sx + x, sy + y, allocation.width, allocation.height);
  g_autofree gchar* executable = g_file_read_link("/proc/self/exe", nullptr);
  ASSERT_NE(executable, nullptr);
  auto launch = [&](const gchar* method, ClientResult* result) -> GSubprocess* {
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_ADDRESS", address,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_BUS", destination,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_RECT", rectangle,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_METHOD", method, TRUE);
    GSubprocess* child = g_subprocess_launcher_spawn(
        launcher, nullptr, executable,
        "--gtest_filter=FlViewAccessibilityIntegrationTest.ExternalClient",
        nullptr);
    if (child != nullptr)
      g_subprocess_wait_check_async(child, nullptr, client_done, result);
    return child;
  };
  ClientResult geometry, role;
  Heartbeat heartbeat;
  guint timer = g_timeout_add(
      10,
      [](gpointer data) -> gboolean {
        Heartbeat* beat = static_cast<Heartbeat*>(data);
        gint64 now = g_get_monotonic_time();
        beat->longest = MAX(beat->longest, now - beat->last);
        beat->last = now;
        beat->ticks++;
        return G_SOURCE_CONTINUE;
      },
      &heartbeat);
  g_autoptr(GSubprocess) geometry_client = launch("geometry", &geometry);
  g_autoptr(GSubprocess) role_client = launch("role", &role);
  ASSERT_NE(geometry_client, nullptr);
  ASSERT_NE(role_client, nullptr);
  while (!geometry.done || !role.done) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(1000);
  }
  heartbeat.longest =
      MAX(heartbeat.longest, g_get_monotonic_time() - heartbeat.last);
  g_source_remove(timer);
  EXPECT_TRUE(geometry.success);
  EXPECT_TRUE(role.success);
  EXPECT_GT(heartbeat.ticks, 0u);
  EXPECT_LT(heartbeat.longest, G_TIME_SPAN_SECOND);
  gtk_widget_destroy(window);
  g_object_unref(window);
  EXPECT_EQ(gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)), nullptr);
}

}  // namespace
