#pragma once
// WebViewWidget: GTK4 widget wrapping MalibuView

#include <gtk/gtk.h>
#include <malibu_view.h>
#include <string>
#include <functional>

class WebViewWidget {
public:
    WebViewWidget();
    ~WebViewWidget();

    // Get the GTK widget for embedding
    GtkWidget* widget() const { return drawing_area_; }

    // Load a URL (http/https) or search query
    void load_url(const std::string& url);

    // Navigation
    void reload();
    bool go_back();
    bool go_forward();

    // Get current URL
    std::string current_url() const;

    // Set network handler (called before first load)
    void set_network_handler(std::function<bool(const std::string&, malibu::network::FetchResponse&)> handler);

    // Called when URL changes (for address bar update)
    using UrlChangeCallback = std::function<void(const std::string&)>;
    void set_url_change_callback(UrlChangeCallback cb) { url_change_cb_ = std::move(cb); }

private:
    static void on_draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data);
    static void on_resize(GtkDrawingArea* area, int width, int height, gpointer user_data);
    static gboolean on_mouse_press(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_mouse_release(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_mouse_move(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_scroll(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_key_press(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_key_release(GtkWidget* widget, GdkEvent* event, gpointer user_data);

    void render_to_cairo(cairo_t* cr, int width, int height);
    void pump_tasks();

    MalibuView* view_ = nullptr;
    GtkWidget* drawing_area_ = nullptr;

    int current_width_ = 1024;
    int current_height_ = 768;
    float scroll_y_ = 0.0f;

    std::function<bool(const std::string&, malibu::network::FetchResponse&)> network_handler_;
    UrlChangeCallback url_change_cb_;

    // Request handler callback for MalibuView
    static int request_handler_trampoline(const char* url, void* out_ptr, void* user_data);
};
