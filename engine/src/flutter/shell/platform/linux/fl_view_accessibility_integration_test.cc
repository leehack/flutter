// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <gtk/gtk.h>
#include <unistd.h>

#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "flutter/shell/platform/linux/fl_view_private.h"
#include "flutter/testing/testing.h"
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
    GTEST_SKIP() << "Only invoked by mapped accessibility tests";
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

// Keep cleanup on assertion failures local to this test, including connections
// whose callback data belongs to the stack.
struct ScopedWindow {
  GtkWidget* widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);

  ScopedWindow() { g_object_ref_sink(widget); }
  ~ScopedWindow() { Destroy(); }

  void Destroy() {
    if (widget != nullptr) {
      gtk_widget_destroy(widget);
      g_clear_object(&widget);
    }
  }
};

struct ScopedSignal {
  GObject* object;
  gulong handler;

  ScopedSignal(GObject* source, gulong signal_handler)
      : object(G_OBJECT(g_object_ref(source))), handler(signal_handler) {}
  ~ScopedSignal() { Disconnect(); }

  void Disconnect() {
    if (object != nullptr) {
      if (g_signal_handler_is_connected(object, handler)) {
        g_signal_handler_disconnect(object, handler);
      }
      g_clear_object(&object);
    }
  }
};

struct RealEngineObservation {
  gboolean first_frame = FALSE;
  gboolean root_received = FALSE;
  guint updates = 0;
  FlutterViewId view_id = -1;
};

static void engine_semantics_received(FlEngine*,
                                      const FlutterSemanticsUpdate2* update,
                                      RealEngineObservation* observation) {
  observation->updates++;
  observation->view_id = update->view_id;
  for (size_t i = 0; i < update->node_count; i++) {
    if (update->nodes[i]->id == 0 &&
        g_strcmp0(update->nodes[i]->label, "engine-root") == 0) {
      observation->root_received = TRUE;
    }
  }
}

// Each worker only waits for its newly exec'd child. GTK and its bridge remain
// on the main thread, which continues dispatching while both clients run.
struct ExternalClientWait {
  GSubprocess* process;
  gint done = FALSE;
  gboolean success = FALSE;
};

static gpointer wait_for_external_client(gpointer data) {
  ExternalClientWait* observation = static_cast<ExternalClientWait*>(data);
  g_autoptr(GError) error = nullptr;
  observation->success =
      g_subprocess_wait_check(observation->process, nullptr, &error);
  g_atomic_int_set(&observation->done, TRUE);
  return nullptr;
}

static void check_real_view_external_geometry(GtkWidget* view) {
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

  g_autofree gchar* destination = nullptr;
  gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  while (destination == nullptr && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    g_autoptr(GVariant) names =
        call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
             "org.freedesktop.DBus", "ListNames");
    ASSERT_NE(names, nullptr);
    g_auto(GStrv) values = nullptr;
    g_variant_get(names, "(^as)", &values);
    for (guint i = 0; values[i] != nullptr; i++) {
      if (values[i][0] != ':' ||
          g_strcmp0(values[i], g_dbus_connection_get_unique_name(bus)) == 0) {
        continue;
      }
      g_autoptr(GVariant) pid_reply =
          call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
               "org.freedesktop.DBus", "GetConnectionUnixProcessID",
               g_variant_new("(s)", values[i]));
      ASSERT_NE(pid_reply, nullptr);
      guint pid;
      g_variant_get(pid_reply, "(u)", &pid);
      if (pid == static_cast<guint>(getpid())) {
        destination = g_strdup(values[i]);
        break;
      }
    }
    g_usleep(1000);
  }
  ASSERT_NE(destination, nullptr);
  ASSERT_TRUE(gtk_widget_get_mapped(view));
  GtkWidget* window = gtk_widget_get_toplevel(view);
  gint x, y, sx, sy;
  ASSERT_TRUE(gtk_widget_translate_coordinates(view, window, 0, 0, &x, &y));
  gdk_window_get_origin(gtk_widget_get_window(window), &sx, &sy);
  GtkAllocation allocation;
  gtk_widget_get_allocation(view, &allocation);
  g_autofree gchar* rectangle = g_strdup_printf(
      "%d,%d,%d,%d", sx + x, sy + y, allocation.width, allocation.height);
  g_autofree gchar* executable = g_file_read_link("/proc/self/exe", nullptr);
  ASSERT_NE(executable, nullptr);
  auto launch = [&](const gchar* method) -> GSubprocess* {
    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_ADDRESS", address,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_BUS", destination,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_RECT", rectangle,
                                 TRUE);
    g_subprocess_launcher_setenv(launcher, "FL_A11Y_TEST_METHOD", method, TRUE);
    return g_subprocess_launcher_spawn(
        launcher, nullptr, executable,
        "--gtest_filter=FlViewAccessibilityIntegrationTest.ExternalClient",
        nullptr);
  };
  g_autoptr(GSubprocess) geometry_client = launch("geometry");
  g_autoptr(GSubprocess) role_client = launch("role");
  if (geometry_client == nullptr || role_client == nullptr) {
    if (geometry_client != nullptr) {
      g_subprocess_force_exit(geometry_client);
      g_subprocess_wait(geometry_client, nullptr, nullptr);
    }
    if (role_client != nullptr) {
      g_subprocess_force_exit(role_client);
      g_subprocess_wait(role_client, nullptr, nullptr);
    }
    FAIL() << "Failed to launch external AT-SPI clients";
  }
  ExternalClientWait geometry{geometry_client}, role{role_client};
  GThread* geometry_waiter =
      g_thread_new("a11y-geometry", wait_for_external_client, &geometry);
  GThread* role_waiter =
      g_thread_new("a11y-role", wait_for_external_client, &role);
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
  deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
  while ((!g_atomic_int_get(&geometry.done) || !g_atomic_int_get(&role.done)) &&
         g_get_monotonic_time() < deadline) {
    g_main_context_iteration(nullptr, FALSE);
    g_usleep(1000);
  }
  gboolean completed =
      g_atomic_int_get(&geometry.done) && g_atomic_int_get(&role.done);
  if (!completed) {
    if (!g_atomic_int_get(&geometry.done)) {
      g_subprocess_force_exit(geometry_client);
    }
    if (!g_atomic_int_get(&role.done)) {
      g_subprocess_force_exit(role_client);
    }
  }
  // Reap before stack-owned observations or subprocess references disappear.
  g_thread_join(geometry_waiter);
  g_thread_join(role_waiter);
  heartbeat.longest =
      MAX(heartbeat.longest, g_get_monotonic_time() - heartbeat.last);
  g_source_remove(timer);
  EXPECT_TRUE(completed) << "External AT-SPI clients exceeded eight seconds";
  EXPECT_TRUE(geometry.success);
  EXPECT_TRUE(role.success);
  EXPECT_GT(heartbeat.ticks, 0u);
  EXPECT_LT(heartbeat.longest, G_TIME_SPAN_SECOND);
}

TEST_F(FlViewAccessibilityIntegrationTest, MappedFlViewEngineSemantics) {
  ASSERT_TRUE(gtk_init_check(nullptr, nullptr));
  // FLUTTER_LINUX_RENDERER is read when the real FlEngine is constructed.
  // Keep the software-renderer warning visible in the runtime receipt.
  g_autofree gchar* previous_renderer =
      g_strdup(g_getenv("FLUTTER_LINUX_RENDERER"));
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_enable_impeller(project, FALSE);
  g_autofree gchar* assets = g_strdup(flutter::testing::GetFixturesPath());
  g_autofree gchar* icu = g_build_filename(assets, "icudtl.dat", nullptr);
  g_autofree gchar* kernel =
      g_build_filename(assets, "kernel_blob.bin", nullptr);
  g_autofree gchar* aot =
      g_build_filename(assets, "app_elf_snapshot.so", nullptr);
  ASSERT_TRUE(g_file_test(kernel, G_FILE_TEST_IS_REGULAR));
  ASSERT_TRUE(g_file_test(icu, G_FILE_TEST_IS_REGULAR));
  fl_dart_project_set_assets_path(project, assets);
  fl_dart_project_set_icu_data_path(project, icu);
  fl_dart_project_set_aot_library_path(project, aot);

  RealEngineObservation observation;
  ScopedWindow window;
  GtkWidget* fixed = gtk_fixed_new();
  gtk_container_add(GTK_CONTAINER(window.widget), fixed);
  g_setenv("FLUTTER_LINUX_RENDERER", "software", TRUE);
  FlView* view = fl_view_new(project);
  if (previous_renderer != nullptr) {
    g_setenv("FLUTTER_LINUX_RENDERER", previous_renderer, TRUE);
  } else {
    g_unsetenv("FLUTTER_LINUX_RENDERER");
  }
  gtk_widget_set_size_request(GTK_WIDGET(view), 280, 180);
  gtk_fixed_put(GTK_FIXED(fixed), GTK_WIDGET(view), 31, 47);
  g_autoptr(FlEngine) engine =
      FL_ENGINE(g_object_ref(fl_view_get_engine(view)));
  g_autoptr(FlViewAccessible) accessible = FL_VIEW_ACCESSIBLE(
      g_object_ref(gtk_widget_get_accessible(GTK_WIDGET(view))));
  ASSERT_EQ(accessible, fl_view_get_accessible(view));
  atk_object_set_name(ATK_OBJECT(accessible), "a11y-view");
  ScopedSignal semantics_signal(
      G_OBJECT(engine),
      g_signal_connect(engine, "update-semantics",
                       G_CALLBACK(engine_semantics_received), &observation));
  ScopedSignal frame_signal(
      G_OBJECT(view),
      g_signal_connect(
          view, "first-frame",
          G_CALLBACK(+[](FlView*, RealEngineObservation* received) {
            received->first_frame = TRUE;
          }),
          &observation));

  // Realizing the actual FlView starts its real engine. The Dart fixture,
  // rather than this test, creates semantics and submits them to that engine.
  gtk_widget_show_all(window.widget);
  auto wait_for = [](auto condition, gint64 duration) -> gboolean {
    gint64 deadline = g_get_monotonic_time() + duration;
    while (!condition() && g_get_monotonic_time() < deadline) {
      g_main_context_iteration(nullptr, FALSE);
      g_usleep(1000);
    }
    return condition();
  };
  ASSERT_TRUE(wait_for(
      [&]() {
        return observation.first_frame && observation.root_received &&
               atk_object_get_n_accessible_children(ATK_OBJECT(accessible)) ==
                   1;
      },
      10 * G_TIME_SPAN_SECOND));
  EXPECT_GT(observation.updates, 0u);
  EXPECT_EQ(observation.view_id, fl_view_get_id(view));
  ASSERT_TRUE(gtk_widget_get_mapped(GTK_WIDGET(view)));
  g_autoptr(AtkObject) root =
      atk_object_ref_accessible_child(ATK_OBJECT(accessible), 0);
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(atk_object_get_name(root), "engine-root");
  g_autoptr(AtkObject) child = atk_object_ref_accessible_child(root, 0);
  ASSERT_NE(child, nullptr);
  EXPECT_STREQ(atk_object_get_name(child), "engine-child");
  EXPECT_EQ(atk_object_get_parent(root), ATK_OBJECT(accessible));
  EXPECT_EQ(atk_object_get_parent(child), root);
  gint x, y, width, height;
  atk_component_get_extents(ATK_COMPONENT(child), &x, &y, &width, &height,
                            ATK_XY_PARENT);
  EXPECT_EQ(x, 11);
  EXPECT_EQ(y, 13);
  EXPECT_EQ(width, 60);
  EXPECT_EQ(height, 40);
  ASSERT_NO_FATAL_FAILURE(check_real_view_external_geometry(GTK_WIDGET(view)));

  // This enters the real engine's action path and waits for Dart to report the
  // event through a second, engine-generated semantics update.
  ASSERT_EQ(atk_action_get_n_actions(ATK_ACTION(child)), 1);
  ASSERT_STREQ(atk_action_get_name(ATK_ACTION(child), 0), "Tap");
  ASSERT_TRUE(atk_action_do_action(ATK_ACTION(child), 0));
  ASSERT_TRUE(wait_for(
      [&]() {
        return g_strcmp0(atk_object_get_name(child), "action-received") == 0;
      },
      5 * G_TIME_SPAN_SECOND));
  g_autoptr(AtkObject) updated_child = atk_object_ref_accessible_child(root, 0);
  EXPECT_EQ(updated_child, child);

  frame_signal.Disconnect();
  semantics_signal.Disconnect();
  window.Destroy();
  g_clear_object(&engine);
  ASSERT_TRUE(wait_for(
      [&]() {
        return gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)) == nullptr;
      },
      5 * G_TIME_SPAN_SECOND));
  g_autoptr(AtkStateSet) detached =
      atk_object_ref_state_set(ATK_OBJECT(accessible));
  EXPECT_TRUE(atk_state_set_contains_state(detached, ATK_STATE_DEFUNCT));
}

}  // namespace
