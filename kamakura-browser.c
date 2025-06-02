#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <glib.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>

#define APP_FLAGS G_APPLICATION_FLAGS_NONE
#define NUM_CHILDREN 128

//init config
const gchar *DEFAULT_HOME_PAGE = "https://www.duckduckgo.com";
const gchar *DEFAULT_SEARCH_ENGINE_URL = "https://duckduckgo.com/?q=%s";


//TrieNode def.
typedef struct TrieNode {
    bool is_end;
    struct TrieNode *children[NUM_CHILDREN];
} TrieNode;

//declare AppData and TabData because they might reference each other indirectly
typedef struct _AppData AppData;
typedef struct _TabData TabData;

//kamakura data struct
struct _AppData {
    GtkWidget *main_window;
    GtkWidget *notebook;
    GtkWidget *url_entry;
    TrieNode *blocked_domains_trie;
    GtkApplication *app_instance;
};

//per-tab data struct
struct _TabData {
    AppData *app;
    GtkWidget *container;
    WebKitWebView *web_view;
    GtkWidget *tab_label_hbox;
    GtkWidget *tab_icon_image;
    GtkWidget *tab_title_label;
};

//trie func.
TrieNode *create_node(void);
static inline char to_lower_char(char c);
void insert_domain(TrieNode *root, const char *domain);
bool search_domain(TrieNode *root, const char *hostname);
void free_trie(TrieNode *node);
void load_trie_block_list(TrieNode *root, const char *filename);
static TabData* get_active_tab(AppData *app);
static GtkWidget* create_tab_label(TabData *tab_data);
static gboolean is_likely_url(const gchar *text);
static void save_history(const gchar *uri);
static TabData* create_new_tab(AppData *app, const gchar *uri_or_search_term);
static WebKitWebView* on_web_view_create_new_window(WebKitWebView *web_view, WebKitNavigationAction *navigation_action, gpointer user_data);
static gboolean on_script_dialog(WebKitWebView *web_view, WebKitScriptDialog *dialog, gpointer user_data);
static gboolean decide_policy_callback(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer user_data);
static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data);
static void on_notify_favicon(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data);
//ui callbacks
static void on_tab_close_clicked(GtkButton *button, gpointer user_data);
static void on_back_clicked(GtkButton *button, gpointer user_data);
static void on_forward_clicked(GtkButton *button, gpointer user_data);
static void on_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_stop_clicked(GtkButton *button, gpointer user_data);
static void on_go_activated(GtkWidget *widget, gpointer user_data);
static void on_new_tab_clicked(GtkButton *button, gpointer user_data);
//context menu callbacks
static gboolean on_web_view_context_menu(WebKitWebView *web_view, GtkWidget *default_menu, WebKitHitTestResult *hit_test_result, gboolean triggered_with_keyboard, gpointer user_data);
static void on_inspect_element_activated(GtkMenuItem *menuitem, gpointer user_data); //corrected
//keyboard shortcuts & zoom callbacks
static gboolean on_key_press_main_window(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void change_zoom_level(AppData *app, gdouble delta);
//main application lifecycle
static void app_shutdown_cleanup(GtkWidget *widget, gpointer user_data); //changed for GtkWindow "destroy"
static void activate(GtkApplication *app_instance, gpointer user_data);


TrieNode *create_node(void) {
    TrieNode *node = malloc(sizeof(TrieNode));
    if (!node) return NULL;
    node->is_end = false;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        node->children[i] = NULL;
    }
    return node;
}

static inline char to_lower_char(char c) {
    return (char)tolower((unsigned char)c);
}

void insert_domain(TrieNode *root, const char *domain) {
    if (!root || !domain) return;
    int len = strlen(domain);
    TrieNode *node = root;
    for (int i = len - 1; i >= 0; i--) {
        char ch = to_lower_char(domain[i]);
        int index = (int)ch;
        if (index < 0 || index >= NUM_CHILDREN) {
            g_warning("Invalid char in domain: %c for trie insertion", ch);
            continue;
        }
        if (node->children[index] == NULL) {
            node->children[index] = create_node();
            if (!node->children[index]) {
                g_warning("Trie node creation failed during insert");
                return;
            }
        }
        node = node->children[index];
    }
    node->is_end = true;
}

bool search_domain(TrieNode *root, const char *hostname) {
    if (!root || !hostname) return false;
    int len = strlen(hostname);
    TrieNode *node = root;
    for (int i = len - 1; i >= 0; i--) {
        char ch = to_lower_char(hostname[i]);
        int index = (int)ch;
        if (index < 0 || index >= NUM_CHILDREN) {
            return false; //invalid char cannot be in trie
        }
        if (node->children[index] == NULL) {
            return false;
        }
        node = node->children[index];
        if (node->is_end) {
            return true;
        }
    }
    return false;
}

void free_trie(TrieNode *node) {
    if (!node) return;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (node->children[i]) {
            free_trie(node->children[i]);
        }
    }
    free(node);
}

void load_trie_block_list(TrieNode *root, const char *filename) {
    gchar *contents;
    gsize length;
    GError *error = NULL;
    if (!g_file_get_contents(filename, &contents, &length, &error)) {
        g_warning("Failed to read block list file %s: %s", filename, error->message);
        g_error_free(error);
        return;
    }
    gchar **lines = g_strsplit(contents, "\n", -1);
    if (lines) {
        for (int i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (line[0] == '\0' || line[0] == '#') continue;
            gchar **tokens = g_strsplit_set(line, " \t", 0);
            if (tokens && tokens[0] && tokens[1]) {
                insert_domain(root, tokens[1]);
            }
            g_strfreev(tokens);
        }
        g_strfreev(lines);
    }
    g_free(contents);
}


static TabData* get_active_tab(AppData *app) {
    if (!app || !app->notebook) return NULL;
    gint current_page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (current_page_num < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), current_page_num);
    if (!page) return NULL;
    return (TabData *)g_object_get_data(G_OBJECT(page), "tab-data");
}

static GtkWidget* create_tab_label(TabData *tab_data) {
    tab_data->tab_label_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    tab_data->tab_icon_image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(tab_data->tab_icon_image), 16);
    gtk_box_pack_start(GTK_BOX(tab_data->tab_label_hbox), tab_data->tab_icon_image, FALSE, FALSE, 0);

    tab_data->tab_title_label = gtk_label_new("New Tab");
    gtk_widget_set_hexpand(tab_data->tab_title_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(tab_data->tab_title_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(tab_data->tab_label_hbox), tab_data->tab_title_label, TRUE, TRUE, 0);

    GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), tab_data->container);
    gtk_box_pack_start(GTK_BOX(tab_data->tab_label_hbox), close_button, FALSE, FALSE, 0);

    gtk_widget_show_all(tab_data->tab_label_hbox);
    return tab_data->tab_label_hbox;
}

static gboolean is_likely_url(const gchar *text) {
    if (!text || *text == '\0') return FALSE;
    if (g_str_has_prefix(text, "http://") || g_str_has_prefix(text, "https://") ||
        g_str_has_prefix(text, "file://") || g_str_has_prefix(text, "about:") ||
        (strstr(text, ".") != NULL && strchr(text, ' ') == NULL) ||
        (g_strcmp0(text, "localhost") == 0) ||
        (strchr(text, ':') != NULL && strstr(text, " ") == NULL && strstr(text, "://") == NULL && strstr(text, ".") != NULL && g_uri_is_valid(text, G_URI_FLAGS_NONE, NULL)) ) {
        return TRUE;
    }
    return FALSE;
}

static void save_history(const gchar *uri) {
    if (!uri || g_strcmp0(uri, "about:blank") == 0) return;
    FILE *file = fopen("history.txt", "a");
    if (file) { fprintf(file, "%s\n", uri); fclose(file);
    } else { g_warning("Failed to open history.txt for appending."); }
}

//webview callback
static WebKitWebView* on_web_view_create_new_window(WebKitWebView *web_view G_GNUC_UNUSED, WebKitNavigationAction *navigation_action, gpointer user_data) {
    AppData *app = ((TabData*)user_data)->app;
    const gchar *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(navigation_action));
    g_message("New window requested for: %s. Opening in new tab.", uri ? uri : "(unknown URI)");
    create_new_tab(app, uri);
    return NULL;
}

static gboolean on_script_dialog(WebKitWebView *web_view G_GNUC_UNUSED, WebKitScriptDialog *dialog, gpointer user_data G_GNUC_UNUSED) {
    g_message("Script dialog (alert, prompt, confirm) blocked.");
    webkit_script_dialog_close(dialog);
    return TRUE;
}

//tab create here
static TabData* create_new_tab(AppData *app, const gchar *uri_or_search_term) {
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());

    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_encrypted_media(settings, TRUE);
    webkit_settings_set_enable_fullscreen(settings, TRUE);
    webkit_settings_set_enable_javascript(settings, TRUE);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

#ifdef WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE
    g_message("WebKit compile-time flag WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE is set.");
    webkit_settings_set_enable_mediasource(settings, TRUE);
#else
    g_message("WebKit compile-time flag WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE is NOT set.");
#endif
    webkit_settings_set_enable_accelerated_2d_canvas(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, TRUE);
    webkit_settings_set_enable_offline_web_application_cache(settings, TRUE); 
    webkit_settings_set_enable_dns_prefetching(settings, TRUE); 
    webkit_settings_set_enable_html5_database(settings, TRUE);
    webkit_settings_set_enable_html5_local_storage(settings, TRUE);

    const gchar *user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.0.0 Safari/537.36 KamakuraBrowser/1.0";
    webkit_settings_set_user_agent(settings, user_agent);
    webkit_web_view_set_settings(web_view, settings);
    g_object_unref(settings);

    WebKitWebContext *context = webkit_web_view_get_context(web_view);
    WebKitCookieManager *cookie_manager = webkit_web_context_get_cookie_manager(context);
    webkit_cookie_manager_set_persistent_storage(cookie_manager, "cookies.txt", WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
    webkit_cookie_manager_set_accept_policy(cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

    gtk_box_pack_start(GTK_BOX(container), GTK_WIDGET(web_view), TRUE, TRUE, 0);

    TabData *tab_data = g_malloc0(sizeof(TabData));
    tab_data->app = app;
    tab_data->container = container;
    tab_data->web_view = web_view;

    g_object_set_data(G_OBJECT(container), "tab-data", tab_data);

    g_signal_connect(web_view, "create", G_CALLBACK(on_web_view_create_new_window), tab_data);
    g_signal_connect(web_view, "script-dialog", G_CALLBACK(on_script_dialog), NULL);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(decide_policy_callback), tab_data);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), tab_data);
    g_signal_connect(web_view, "context-menu", G_CALLBACK(on_web_view_context_menu), tab_data);
    g_signal_connect(web_view, "notify::favicon", G_CALLBACK(on_notify_favicon), tab_data);
    
    GtkWidget *custom_tab_label_widget = create_tab_label(tab_data);

    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), container, custom_tab_label_widget);
    gtk_widget_show_all(container);

    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), container);
    if (page_num != -1) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), page_num);
    }
    gtk_widget_grab_focus(GTK_WIDGET(web_view));

    if (uri_or_search_term && *uri_or_search_term) {
        gchar *final_uri_to_load = NULL;
        if (is_likely_url(uri_or_search_term)) {
            if (!g_str_has_prefix(uri_or_search_term, "http://") &&
                !g_str_has_prefix(uri_or_search_term, "https://") &&
                !g_str_has_prefix(uri_or_search_term, "file://") &&
                !g_str_has_prefix(uri_or_search_term, "about:")) {
                final_uri_to_load = g_strdup_printf("http://%s", uri_or_search_term);
            } else {
                final_uri_to_load = g_strdup(uri_or_search_term);
            }
        } else {
            gchar *escaped_query = g_uri_escape_string(uri_or_search_term, NULL, TRUE);
            final_uri_to_load = g_strdup_printf(DEFAULT_SEARCH_ENGINE_URL, escaped_query);
            g_free(escaped_query);
        }
        webkit_web_view_load_uri(web_view, final_uri_to_load);
        g_free(final_uri_to_load);
    } else {
        webkit_web_view_load_uri(web_view, DEFAULT_HOME_PAGE);
    }
    return tab_data;
}


static void on_load_changed(WebKitWebView *web_view_param G_GNUC_UNUSED, WebKitLoadEvent load_event, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (!tab || !tab->tab_title_label) return;

    //tab->web_view instead of web_view_param to be sure
    const gchar *uri = webkit_web_view_get_uri(tab->web_view); 
    
    AppData *app_ptr = tab->app;
    if (app_ptr && tab == get_active_tab(app_ptr)) {
         gtk_entry_set_text(GTK_ENTRY(app_ptr->url_entry), uri ? uri : "");
    }

    if (load_event == WEBKIT_LOAD_FINISHED) {
        save_history(uri);
        const gchar *original_title = webkit_web_view_get_title(tab->web_view);
        gchar *processed_title = NULL;
        gboolean title_is_allocated = FALSE;

        if (original_title && *original_title) {
            processed_title = g_strdup(original_title);
            title_is_allocated = TRUE;
        } else if (uri && (g_strcmp0(uri, "about:blank") == 0 || g_strcmp0(uri, "") == 0)) {
            processed_title = g_strdup("New Tab");
            title_is_allocated = TRUE;
        } else if (uri) {
            GUri *temp_guri = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
            if (temp_guri) {
                const char *host = g_uri_get_host(temp_guri);
                processed_title = g_strdup((host && *host) ? host : uri);
                g_uri_unref(temp_guri);
            } else {
                processed_title = g_strdup(uri ? uri : "Untitled");
            }
            title_is_allocated = TRUE;
        } else {
            processed_title = g_strdup("Untitled");
            title_is_allocated = TRUE;
        }
        
        const int MAX_TITLE_LENGTH = 25; 
        char short_title_buffer[MAX_TITLE_LENGTH + 3 + 1]; //for "..." and null terminator
        memset(short_title_buffer, 0, sizeof(short_title_buffer));

        if (strlen(processed_title) > (size_t)MAX_TITLE_LENGTH) {
            gchar *temp_short_substr = g_utf8_substring(processed_title, 0, MAX_TITLE_LENGTH);
            g_snprintf(short_title_buffer, sizeof(short_title_buffer), "%s...", temp_short_substr);
            g_free(temp_short_substr);
        } else {
            g_strlcpy(short_title_buffer, processed_title, sizeof(short_title_buffer));
        }
        gtk_label_set_text(GTK_LABEL(tab->tab_title_label), short_title_buffer);

        if (title_is_allocated) {
            g_free(processed_title);
        }

    } else if (load_event == WEBKIT_LOAD_STARTED) {
         gtk_label_set_text(GTK_LABEL(tab->tab_title_label), "Loading...");
         if (tab->tab_icon_image) {
            gtk_image_set_from_icon_name(GTK_IMAGE(tab->tab_icon_image), "image-loading-symbolic", GTK_ICON_SIZE_MENU);
         }
    }
}

static void on_notify_favicon(WebKitWebView *web_view, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (!tab || !tab->tab_icon_image) return;

    cairo_surface_t *favicon_surface = webkit_web_view_get_favicon(web_view);
    if (favicon_surface) { //cairo_surface_status(favicon_surface) == CAIRO_STATUS_SUCCESS
        int width = cairo_image_surface_get_width(favicon_surface);
        int height = cairo_image_surface_get_height(favicon_surface);
        if (width > 0 && height > 0) { //ensure valid dimensions
            GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(favicon_surface, 0, 0, width, height);
            if (pixbuf) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(tab->tab_icon_image), pixbuf);
                g_object_unref(pixbuf);
            } else { //fallback if pixbuf creation fails
                 gtk_image_set_from_icon_name(GTK_IMAGE(tab->tab_icon_image), "text-html-symbolic", GTK_ICON_SIZE_MENU);
            }
        } else { //fallback if dimensions are invalid
             gtk_image_set_from_icon_name(GTK_IMAGE(tab->tab_icon_image), "text-html-symbolic", GTK_ICON_SIZE_MENU);
        }
        //webKit owns the cairo_surface_t returned by webkit_web_view_get_favicon()
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(tab->tab_icon_image), "text-html-symbolic", GTK_ICON_SIZE_MENU);
    }
}

static gboolean decide_policy_callback(WebKitWebView *web_view_param G_GNUC_UNUSED, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    AppData *app = tab->app;
    const gchar *uri = NULL;
    WebKitURIRequest *request = NULL;

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *nav_action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        request = webkit_navigation_action_get_request(nav_action);
        uri = webkit_uri_request_get_uri(request);

        guint button = webkit_navigation_action_get_mouse_button(nav_action);
        WebKitNavigationType nav_type = webkit_navigation_action_get_navigation_type(nav_action);

        if (button == 2 && nav_type == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) { 
            if (uri) {
                g_message("Middle-click on link: %s. Opening in new tab.", uri);
                create_new_tab(app, uri);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
        }
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *resp_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        request = webkit_response_policy_decision_get_request(resp_decision);
        uri = webkit_uri_request_get_uri(request);
    } else { 
        return FALSE;
    }

    if (request && uri) {
        GError *err = NULL;
        GUri *parsed_g_uri = g_uri_parse(uri, G_URI_FLAGS_NONE, &err);
        if (parsed_g_uri) {
            const gchar *host = g_uri_get_host(parsed_g_uri);
            if (host) {
                static const char *hardcoded_domains[] = { "doubleclick.net", "googlesyndication.com", "adservice.google.com", NULL };
                for (int i = 0; hardcoded_domains[i] != NULL; i++) {
                    if (strstr(host, hardcoded_domains[i]) != NULL) { 
                        g_print("Blocking (hardcoded): %s on host %s\n", uri, host);
                        webkit_policy_decision_ignore(decision);
                        g_uri_unref(parsed_g_uri);
                        return TRUE;
                    }
                }
                if (app->blocked_domains_trie && search_domain(app->blocked_domains_trie, host)) { //search_domain is now forward-declared
                    g_print("Blocking (trie): %s on host %s\n", uri, host);
                    webkit_policy_decision_ignore(decision);
                    g_uri_unref(parsed_g_uri);
                    return TRUE;
                }
            }
            g_uri_unref(parsed_g_uri);
        } else if (err) {
            g_warning("Failed to parse URI (%s): %s", uri, err->message);
            g_error_free(err);
        }
    }
    return FALSE;
}


static void on_back_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) { AppData *app = (AppData *)user_data; TabData *tab = get_active_tab(app); if (tab && webkit_web_view_can_go_back(tab->web_view)) webkit_web_view_go_back(tab->web_view); }
static void on_forward_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) { AppData *app = (AppData *)user_data; TabData *tab = get_active_tab(app); if (tab && webkit_web_view_can_go_forward(tab->web_view)) webkit_web_view_go_forward(tab->web_view); }
static void on_refresh_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) { AppData *app = (AppData *)user_data; TabData *tab = get_active_tab(app); if (tab) webkit_web_view_reload(tab->web_view); }
static void on_stop_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) { AppData *app = (AppData *)user_data; TabData *tab = get_active_tab(app); if (tab) webkit_web_view_stop_loading(tab->web_view); }

static void on_go_activated(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    const gchar *uri_text = gtk_entry_get_text(GTK_ENTRY(app->url_entry));
    TabData *tab = get_active_tab(app);
    gchar *final_uri_to_load = NULL;

    if (uri_text && *uri_text) {
        if (is_likely_url(uri_text)) {
            if (!g_str_has_prefix(uri_text, "http://") && !g_str_has_prefix(uri_text, "https://") &&
                !g_str_has_prefix(uri_text, "file://") && !g_str_has_prefix(uri_text, "about:")) {
                final_uri_to_load = g_strdup_printf("http://%s", uri_text);
            } else {
                final_uri_to_load = g_strdup(uri_text);
            }
        } else {
            gchar *escaped_query = g_uri_escape_string(uri_text, NULL, TRUE); // NULL_OK if uri_text can be NULL (though checked before)
            if (escaped_query) {
                 final_uri_to_load = g_strdup_printf(DEFAULT_SEARCH_ENGINE_URL, escaped_query);
                 g_free(escaped_query);
            } else { //should not happen if uri_text is not NULL
                 final_uri_to_load = g_strdup(DEFAULT_HOME_PAGE);
            }
        }
    }

    if (!tab) {
        tab = create_new_tab(app, final_uri_to_load ? final_uri_to_load : NULL); //pass NULL to load homepage -- NULL_OK wrong
    } else if (final_uri_to_load) {
        webkit_web_view_load_uri(tab->web_view, final_uri_to_load);
    } else if (tab) { //no input, and tab exists, do nothing
        // webkit_web_view_load_uri(tab->web_view, DEFAULT_HOME_PAGE);
    }
    
    if (final_uri_to_load) g_free(final_uri_to_load);
    if (tab) gtk_widget_grab_focus(GTK_WIDGET(tab->web_view));
}

static void on_new_tab_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    create_new_tab(app, NULL); //creates a new tab with the default homepage
}

static void on_tab_close_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data) {
    GtkWidget *container_to_close = GTK_WIDGET(user_data);
    TabData *tab_to_close = (TabData*) g_object_get_data(G_OBJECT(container_to_close), "tab-data");
    if (!tab_to_close) return;
    
    AppData *app = tab_to_close->app;
    GtkWidget *notebook = gtk_widget_get_parent(container_to_close);

    if (!GTK_IS_NOTEBOOK(notebook)) { 
        g_warning("Parent of tab container is not a GtkNotebook as expected!"); 
        return; 
    }

    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), container_to_close);
    if (page_num != -1) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    }
    
    //tabdata needs to be freed
    g_object_set_data(G_OBJECT(container_to_close), "tab-data", NULL); //clear the data pointer
    g_free(tab_to_close); //free it here

    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook)) == 0) {
        create_new_tab(app, NULL); //optionally open a new default tab -- comment if u wish not to open tab to duckduckgo
        //or, if it's a GtkApplication, consider g_application_quit or closing the window.
        //gtk_widget_destroy(app->main_window);
    }
}

static void on_inspect_element_activated(GtkMenuItem *menuitem G_GNUC_UNUSED, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (tab && tab->web_view) {
        WebKitWebInspector *inspector = webkit_web_view_get_inspector(tab->web_view);
        webkit_web_inspector_show(inspector);
    }
}

static gboolean on_web_view_context_menu(WebKitWebView *web_view, GtkWidget *default_menu G_GNUC_UNUSED,
                                        WebKitHitTestResult *hit_test_result G_GNUC_UNUSED,
                                        gboolean triggered_with_keyboard G_GNUC_UNUSED, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (!tab) return FALSE;

    WebKitSettings *settings = webkit_web_view_get_settings(web_view);
    if (!webkit_settings_get_enable_developer_extras(settings)) {
        return FALSE;
    }

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *inspect_item = gtk_menu_item_new_with_label("Inspect Element");
    g_signal_connect(inspect_item, "activate", G_CALLBACK(on_inspect_element_activated), tab);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), inspect_item);
    gtk_widget_show(inspect_item);

    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
}

static void change_zoom_level(AppData *app, gdouble delta) {
    TabData *tab = get_active_tab(app);
    if (tab && tab->web_view) {
        gdouble current_level = webkit_web_view_get_zoom_level(tab->web_view);
        if (delta == 0.0) { 
            webkit_web_view_set_zoom_level(tab->web_view, 1.0);
        } else {
            webkit_web_view_set_zoom_level(tab->web_view, current_level + delta);
        }
    }
}

static gboolean on_key_press_main_window(GtkWidget *widget G_GNUC_UNUSED, GdkEventKey *event, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    TabData *tab = get_active_tab(app);

    if (event->keyval == GDK_KEY_F12) {
        if (tab && tab->web_view) {
            WebKitWebInspector *inspector = webkit_web_view_get_inspector(tab->web_view);
            if (webkit_web_inspector_is_attached(inspector)) {
                webkit_web_inspector_close(inspector);
            } else {
                webkit_web_inspector_show(inspector);
            }
            return TRUE;
        }
    } else if (event->state & GDK_CONTROL_MASK) {
        switch (event->keyval) {
            case GDK_KEY_plus: case GDK_KEY_KP_Add:  change_zoom_level(app, 0.1); return TRUE;
            case GDK_KEY_minus: case GDK_KEY_KP_Subtract: change_zoom_level(app, -0.1); return TRUE;
            case GDK_KEY_0: case GDK_KEY_KP_0: change_zoom_level(app, 0.0); return TRUE;
        }
    }
    return FALSE;
}

static void app_shutdown_cleanup(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    if (app) { // Check if app data is valid
        if (app->blocked_domains_trie) {
            g_message("Freeing blocked domains trie on window destroy.");
            free_trie(app->blocked_domains_trie);
            app->blocked_domains_trie = NULL;
        }

        g_free(app);
        //will take some time to shutdown but good to do
    }
}


static void activate(GtkApplication *app_instance, gpointer user_data G_GNUC_UNUSED) {
    AppData *app = g_new0(AppData, 1);
    app->app_instance = app_instance;

    GtkBuilder *builder = gtk_builder_new_from_file("kamakura_browser.glade");
    if (!builder) {
        g_critical("Failed to load kamakura_browser.glade. Exiting.");
        g_application_quit(G_APPLICATION(app_instance)); g_free(app); return;
    }

    app->main_window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    gtk_window_set_application(GTK_WINDOW(app->main_window), app_instance);

    app->notebook      = GTK_WIDGET(gtk_builder_get_object(builder, "notebook"));
    app->url_entry     = GTK_WIDGET(gtk_builder_get_object(builder, "url_entry"));
    GtkWidget *back_button = GTK_WIDGET(gtk_builder_get_object(builder, "back_button"));
    GtkWidget *forward_button = GTK_WIDGET(gtk_builder_get_object(builder, "forward_button"));
    GtkWidget *refresh_button = GTK_WIDGET(gtk_builder_get_object(builder, "refresh_button"));
    GtkWidget *stop_button = GTK_WIDGET(gtk_builder_get_object(builder, "stop_button"));
    GtkWidget *go_button = GTK_WIDGET(gtk_builder_get_object(builder, "go_button"));
    GtkWidget *new_tab_button = GTK_WIDGET(gtk_builder_get_object(builder, "new_tab_button"));
    
    if (!app->main_window || !app->notebook || !app->url_entry || !back_button || !forward_button || !refresh_button || !go_button || !new_tab_button || !stop_button) {
        g_critical("Failed to get all widgets from Glade. Check IDs. Exiting.");
        g_object_unref(builder); g_application_quit(G_APPLICATION(app_instance)); g_free(app); return;
    }

    app->blocked_domains_trie = create_node();
    if (app->blocked_domains_trie) { //check if trie creation was successful
        load_trie_block_list(app->blocked_domains_trie, "block.txt");
    } else {
        g_warning("Failed to create blocked_domains_trie. Ad blocking will not work.");
    }


    g_signal_connect(app->main_window, "destroy", G_CALLBACK(app_shutdown_cleanup), app);
    g_signal_connect(back_button,    "clicked", G_CALLBACK(on_back_clicked), app);
    g_signal_connect(forward_button, "clicked", G_CALLBACK(on_forward_clicked), app);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), app);
    g_signal_connect(go_button,      "clicked", G_CALLBACK(on_go_activated), app);
    g_signal_connect(app->url_entry, "activate", G_CALLBACK(on_go_activated), app);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), app);
    g_signal_connect(app->main_window, "key-press-event", G_CALLBACK(on_key_press_main_window), app);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    GError *css_error = NULL;
    if (gtk_css_provider_load_from_path(css_provider, "style.css", &css_error)) {

        GdkScreen *screen = gtk_widget_get_screen(app->main_window);
        
        gtk_style_context_add_provider_for_screen(screen,
                                                  GTK_STYLE_PROVIDER(css_provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_USER);
    } else {
        g_warning("Failed to load style.css: %s", css_error ? css_error->message : "Unknown error");
        if (css_error) g_error_free(css_error);
    }
    g_object_unref(css_provider);

    create_new_tab(app, NULL); //create initial tab with homepage

    gtk_widget_show_all(app->main_window);
    g_object_unref(builder);
}

int main(int argc, char **argv) {
    GtkApplication *app_instance;
    int status;
    app_instance = gtk_application_new("org.example.KamakuraBrowser", APP_FLAGS);
    g_signal_connect(app_instance, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app_instance), argc, argv);
    g_object_unref(app_instance);
    return status;
}