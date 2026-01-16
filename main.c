#include "config.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static GtkWidget *window = NULL;
static GtkWidget *result_label = NULL;
static int socket_fd = -1;
static GThread *listener_thread = NULL;
static gboolean should_stop = FALSE;
static GMutex window_mutex;

static int check_existing_instance(void) {
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0)
    return 0;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return 0;
  }

  const char *msg = "show\n";
  send(sock, msg, strlen(msg), 0);
  close(sock);
  return 1;
}

static int create_socket(void) {
  socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0)
    return -1;

  unlink(SOCKET_PATH);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

  if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(socket_fd);
    socket_fd = -1;
    return -1;
  }

  if (listen(socket_fd, 5) < 0) {
    close(socket_fd);
    unlink(SOCKET_PATH);
    socket_fd = -1;
    return -1;
  }

  return socket_fd;
}

static gchar *calculate_result(const char *input) {
  if (input == NULL || strlen(input) == 0)
    return NULL;

  char command[1024];
  snprintf(
      command, sizeof(command),
      "echo 'scale=3; %s' | bc -l 2>/dev/null | awk '{printf \"%%g\\n\", $0}'",
      input);

  FILE *fp = popen(command, "r");
  if (fp == NULL)
    return NULL;

  gchar *output = NULL;
  gsize len = 0;
  char buffer[1024];

  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
    gsize buffer_len = strlen(buffer);
    output = g_realloc(output, len + buffer_len + 1);
    memcpy(output + len, buffer, buffer_len);
    len += buffer_len;
    output[len] = '\0';
  }

  pclose(fp);

  if (output != NULL && len > 0 && output[len - 1] == '\n') {
    output[len - 1] = '\0';
  }

  if (output != NULL && strlen(output) == 0) {
    g_free(output);
    return NULL;
  }

  return output;
}

static void on_input_changed(GtkEntry *entry,
                             gpointer user_data G_GNUC_UNUSED) {
  const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  gchar *result = calculate_result(text);

  if (result != NULL) {
    gtk_label_set_text(GTK_LABEL(result_label), result);
  } else {
    gtk_label_set_text(GTK_LABEL(result_label), "");
  }

  g_free(result);
}

static gboolean on_key_press(GtkEventControllerKey *controller G_GNUC_UNUSED,
                             guint keyval, guint keycode G_GNUC_UNUSED,
                             GdkModifierType state G_GNUC_UNUSED,
                             gpointer user_data G_GNUC_UNUSED) {
  if (keyval == GDK_KEY_Escape) {
    gtk_widget_set_visible(window, FALSE);
    return TRUE;
  }
  return FALSE;
}

static gpointer socket_listener_thread(gpointer user_data G_GNUC_UNUSED) {
  while (!should_stop) {
    fd_set readfds;
    struct timeval timeout;
    FD_ZERO(&readfds);
    FD_SET(socket_fd, &readfds);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ret = select(socket_fd + 1, &readfds, NULL, NULL, &timeout);
    if (ret < 0)
      break;
    if (ret == 0)
      continue;

    int client_fd = accept(socket_fd, NULL, NULL);
    if (client_fd < 0)
      continue;

    char buffer[32];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    close(client_fd);

    if (n > 0) {
      buffer[n] = '\0';
      if (strcmp(buffer, "show\n") == 0) {
        g_mutex_lock(&window_mutex);
        if (window != NULL) {
          gtk_widget_set_visible(window, TRUE);
          gtk_window_present(GTK_WINDOW(window));
        }
        g_mutex_unlock(&window_mutex);
      }
    }
  }
  return NULL;
}

static void cleanup_resources(void) {
  should_stop = TRUE;

  if (listener_thread != NULL) {
    g_thread_join(listener_thread);
    listener_thread = NULL;
  }

  if (socket_fd >= 0) {
    close(socket_fd);
    socket_fd = -1;
  }

  unlink(SOCKET_PATH);
}

static void activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED) {
  window = gtk_application_window_new(app);
  gtk_layer_init_for_window(GTK_WINDOW(window));
  gtk_layer_set_namespace(GTK_WINDOW(window), "calculator");
  gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_TOP);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(window),
                              GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);

  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_TOP, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_LEFT, 0);
  gtk_layer_set_margin(GTK_WINDOW(window), GTK_LAYER_SHELL_EDGE_RIGHT, 0);

  gtk_widget_add_css_class(GTK_WIDGET(window), "overlay-window");

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(vbox, FALSE);
  gtk_widget_set_vexpand(vbox, FALSE);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_add_css_class(entry, "input-entry");

  int entry_width = -1;
  if (strstr(INPUT_WIDTH, "%") != NULL) {
    double percent;
    if (sscanf(INPUT_WIDTH, "%lf%%", &percent) == 1) {
      GdkDisplay *display = gdk_display_get_default();
      GListModel *monitors = gdk_display_get_monitors(display);
      if (monitors != NULL && g_list_model_get_n_items(monitors) > 0) {
        GdkMonitor *monitor = g_list_model_get_item(monitors, 0);
        if (monitor != NULL) {
          GdkRectangle geometry;
          gdk_monitor_get_geometry(monitor, &geometry);
          entry_width = (int)(geometry.width * percent / 100.0);
          g_object_unref(monitor);
        }
      }
    }
  } else {
    int pixel_width;
    if (sscanf(INPUT_WIDTH, "%d", &pixel_width) == 1) {
      entry_width = pixel_width;
    }
  }

  if (INPUT_HEIGHT > 0) {
    gtk_widget_set_size_request(entry, entry_width, INPUT_HEIGHT);
  } else if (entry_width > 0) {
    gtk_widget_set_size_request(entry, entry_width, -1);
  }

  result_label = gtk_label_new("");
  gtk_widget_add_css_class(result_label, "result-label");

  gtk_box_append(GTK_BOX(vbox), entry);
  gtk_box_append(GTK_BOX(vbox), result_label);

  GtkEventController *key_controller = gtk_event_controller_key_new();
  g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_press),
                   NULL);
  gtk_widget_add_controller(GTK_WIDGET(window), key_controller);

  g_signal_connect(entry, "changed", G_CALLBACK(on_input_changed), NULL);

  GtkCssProvider *css_provider = gtk_css_provider_new();

  guint bg_r, bg_g, bg_b;
  sscanf(OVERLAY_BACKGROUND_COLOR, "#%02x%02x%02x", &bg_r, &bg_g, &bg_b);

  guint input_r, input_g, input_b;
  sscanf(INPUT_TEXT_COLOR, "#%02x%02x%02x", &input_r, &input_g, &input_b);

  guint result_r, result_g, result_b;
  sscanf(RESULT_TEXT_COLOR, "#%02x%02x%02x", &result_r, &result_g, &result_b);

  guint border_r, border_g, border_b;
  sscanf(INPUT_BORDER_COLOR, "#%02x%02x%02x", &border_r, &border_g, &border_b);

  gchar *border_color = NULL;
  if (INPUT_BORDER_OPACITY < 1.0) {
    border_color = g_strdup_printf("rgba(%u, %u, %u, %f)", border_r, border_g,
                                   border_b, INPUT_BORDER_OPACITY);
  } else {
    border_color =
        g_strdup_printf("#%02x%02x%02x", border_r, border_g, border_b);
  }

  gchar *css = g_strdup_printf(
      "* { outline: none; }"
      ".overlay-window {"
      "  background-color: rgba(%u, %u, %u, %.2f);"
      "}"
      ".input-entry {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(%u, %u, %u, 1.0);"
      "  background-color: rgba(0, 0, 0, 0.3);"
      "  border: %dpx solid %s;"
      "  border-radius: %dpx;"
      "  padding: 0 %dpx;"
      "}"
      ".input-entry:focus { @include entry(normal); }"
      ".result-label {"
      "  font-family: %s;"
      "  font-size: %dpt;"
      "  color: rgba(%u, %u, %u, 1.0);"
      "}",
      bg_r, bg_g, bg_b, OVERLAY_OPACITY, INPUT_FONT, INPUT_FONT_SIZE, input_r,
      input_g, input_b, INPUT_BORDER_WIDTH, border_color, INPUT_BORDER_RADIUS,
      INPUT_PADDING_HORIZONTAL, RESULT_FONT, RESULT_FONT_SIZE, result_r,
      result_g, result_b);

  g_free(border_color);

  gtk_css_provider_load_from_string(css_provider, css);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_free(css);
  g_object_unref(css_provider);

  gtk_window_set_child(GTK_WINDOW(window), vbox);
  gtk_widget_set_visible(window, TRUE);
  gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
  if (check_existing_instance()) {
    return 0;
  }

  if (create_socket() < 0) {
    g_printerr("Failed to create socket\n");
    return 1;
  }

  g_mutex_init(&window_mutex);

  GError *error = NULL;
  listener_thread =
      g_thread_try_new("socket-listener", socket_listener_thread, NULL, &error);
  if (listener_thread == NULL) {
    g_printerr("Failed to create listener thread: %s\n",
               error ? error->message : "Unknown error");
    if (error)
      g_error_free(error);
    cleanup_resources();
    return 1;
  }

  GtkApplication *app =
      gtk_application_new("org.calculator", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(cleanup_resources), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  cleanup_resources();
  g_mutex_clear(&window_mutex);
  g_object_unref(app);

  return status;
}
