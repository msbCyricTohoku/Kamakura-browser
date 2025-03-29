#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <glib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE
    webkit_settings_set_enable_media_source(settings, TRUE);
    webkit_settings_set_enable_accelerated_compositing(settings, TRUE);
#endif



#define G_APPLICATION_DEFAULT_FLAGS 0
#define NUM_CHILDREN 128

/* --- Reversed Domain trie implementation --- */

typedef struct TrieNode {
    bool is_end;                //indicates end of a blocked domain
    struct TrieNode *children[NUM_CHILDREN];
} TrieNode;

//create a new trie node
TrieNode *create_node(void) {
    TrieNode *node = malloc(sizeof(TrieNode));
    if (!node) return NULL;
    node->is_end = false;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        node->children[i] = NULL;
    }
    return node;
}

//convert a character to lowercase (case-insensitive matching)
static inline char to_lower_char(char c) {
    return (char)tolower((unsigned char)c);
}

//insert a domain (in normal order) into the trie in reversed order.
void insert_domain(TrieNode *root, const char *domain) {
    if (!root || !domain)
        return;

    int len = strlen(domain);
    TrieNode *node = root;
    //traverse from the end (reverse the domain)
    for (int i = len - 1; i >= 0; i--) {
        char c = to_lower_char(domain[i]);
        int index = (int)c;
        if (node->children[index] == NULL) {
            node->children[index] = create_node();
        }
        node = node->children[index];
    }
    node->is_end = true;
}

//search for a hostname in the trie by checking its reversed string.
//returns true if the hostname (or any of its parent domains) is blocked.
bool search_domain(TrieNode *root, const char *hostname) {
    if (!root || !hostname)
        return false;

    int len = strlen(hostname);
    TrieNode *node = root;
    for (int i = len - 1; i >= 0; i--) {
        char c = to_lower_char(hostname[i]);
        int index = (int)c;
        if (node->children[index] == NULL) {
            return false;
        }
        node = node->children[index];
        if (node->is_end)
            return true;
    }
    return false;
}

//recursively free the trie.
void free_trie(TrieNode *node) {
    if (!node)
        return;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (node->children[i])
            free_trie(node->children[i]);
    }
    free(node);
}

//load the block list from a file into the trie.
//the file format is assumed to be: "0.0.0.0 domain.name"
void load_trie_block_list(TrieNode *root, const char *filename) {
    gchar *contents;
    gsize length;
    GError *error = NULL;

    if (!g_file_get_contents(filename, &contents, &length, &error)) {
        g_warning("Failed to read block list file %s: %s", filename, error->message);
        g_error_free(error);
        return;
    }

    //split file contents into lines
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (line[0] == '\0' || line[0] == '#')
            continue;  //skip empty or comment lines

        //split each line on whitespace; expected format: "0.0.0.0 domain.name"
        gchar **tokens = g_strsplit_set(line, " \t", 0);
        if (tokens[0] && tokens[1])
            insert_domain(root, tokens[1]);
        g_strfreev(tokens);
    }
    g_strfreev(lines);
    g_free(contents);
}

/* --- End Trie Implementation --- */

/* --- Data Structures for Multi-Tab Browser --- */

//global application data structure
typedef struct _AppData {
    GtkWidget *notebook;
    GtkWidget *url_entry;
    TrieNode *blocked_domains_trie;  //global block list trie
} AppData;

//data for each tab
typedef struct _TabData {
    AppData *app;
    GtkWidget *container;            //the container widget added as a notebook page
    WebKitWebView *web_view;
    GtkWidget *tab_label;            //the label widget used in the tab header
} TabData;

/* Forward declarations for callbacks */
static gboolean decide_policy_callback(WebKitWebView *web_view,
                                         WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType type,
                                         gpointer user_data);
static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data);
static void save_history(const gchar *uri);
static void on_tab_close_clicked(GtkButton *button, gpointer user_data);

/* --- Helper Functions --- */

//get the TabData for the active notebook page.
static TabData* get_active_tab(AppData *app) {
    int current = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (current < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), current);
    return (TabData *)g_object_get_data(G_OBJECT(page), "tab-data");
}

//create a tab label widget (an hbox with a label and a close button)
static GtkWidget* create_tab_label(TabData *tab_data) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *label = gtk_label_new("New Tab");
    tab_data->tab_label = label;  //save pointer to label for later updates

    GtkWidget *close_button = gtk_button_new_with_label("x");
    gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_button, FALSE);
    //when the close button is clicked, pass the tab container as user data.
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), tab_data->container);

    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), close_button, FALSE, FALSE, 0);
    gtk_widget_show_all(hbox);
    return hbox;
}

//popup blocker here
static WebKitWebView* on_web_view_create(WebKitWebView *web_view, WebKitNavigationAction *navigation_action, gpointer user_data) {
    // Optionally print a message or log the blocked pop-up.
    const gchar *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(navigation_action));
    g_print("Popup blocked: %s\n", uri);
    return NULL; //returning NULL blocks the creation of a new WebKitWebView.
}

//popup content on the page block
static gboolean on_script_dialog(WebKitWebView *web_view,
    WebKitScriptDialog *dialog,
    gpointer user_data)
{
//log the blocked dialog if needed.
g_print("Script dialog popup blocked.\n");
//dismiss the dialog immediately.
webkit_script_dialog_close(dialog);
return TRUE;  //returning TRUE prevents the default dialog from showing.
}




//create a new tab with its own web view and add it to the notebook.
static TabData* create_new_tab(AppData *app, const gchar *uri) {
    //create a container for the tab page.
    GtkWidget *container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    //create a new WebKit web view.
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());

    //
    //

    //create and set up WebKit settings.
    WebKitSettings *settings = webkit_settings_new();
    webkit_settings_set_enable_encrypted_media(settings, TRUE);
    webkit_settings_set_enable_fullscreen(settings, TRUE);
    

    //disable / enable JavaScript
    webkit_settings_set_enable_javascript(settings, TRUE);

    const gchar *user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36";
    webkit_settings_set_user_agent(settings, user_agent);
    webkit_web_view_set_settings(web_view, settings);
    g_object_unref(settings);

    //enable cookies and set persistent storage.
    WebKitWebContext *context = webkit_web_view_get_context(web_view);
    WebKitCookieManager *cookie_manager = webkit_web_context_get_cookie_manager(context);
    webkit_cookie_manager_set_persistent_storage(cookie_manager, "cookies.txt", WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);

    //pack the web view into the container.
    gtk_box_pack_start(GTK_BOX(container), GTK_WIDGET(web_view), TRUE, TRUE, 0);

    //allocate and initialize TabData.
    TabData *tab_data = g_malloc(sizeof(TabData));
    tab_data->app = app;
    tab_data->container = container;
    tab_data->web_view = web_view;
    tab_data->tab_label = NULL; //will be set when creating the tab label


    //connection to popup blocker
    g_signal_connect(web_view, "create", G_CALLBACK(on_web_view_create), tab_data);

    //connection to dialog popup
    g_signal_connect(web_view, "script-dialog", G_CALLBACK(on_script_dialog), NULL);


    //associate tab_data with the container widget.
    g_object_set_data(G_OBJECT(container), "tab-data", tab_data);

    //connect signals for this web view.
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(decide_policy_callback), tab_data);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), tab_data);
    


    //create a custom tab label (with a close button)
    GtkWidget *custom_tab_label = create_tab_label(tab_data);

    //add the container as a new page in the notebook with the custom tab label.
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), container, custom_tab_label);
    gtk_widget_show_all(container);

    //switch to the new tab.
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), container);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), page_num);

    //if a URI was provided, load it.
    if (uri && *uri) {
        if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")) {
            gchar *full_uri = g_strdup_printf("http://%s", uri);
            webkit_web_view_load_uri(web_view, full_uri);
            g_free(full_uri);
        } else {
            webkit_web_view_load_uri(web_view, uri);
        }
    }
    return tab_data;
}

/* --- Callback Implementations --- */

//save the visited URI to history.
static void save_history(const gchar *uri) {
    FILE *file = fopen("history.txt", "a");
    if (file) {
        fprintf(file, "%s\n", uri);
        fclose(file);
    }
}

//called when a web view's load state changes.
/*
static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (load_event == WEBKIT_LOAD_FINISHED) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        save_history(uri);
        const gchar *title = webkit_web_view_get_title(web_view);
        if (!title || strlen(title) == 0) {
            title = "New Tab";
        }
        //update the label text in the tab header.
        gtk_label_set_text(GTK_LABEL(tab->tab_label), title);
    }
}
*/

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    if (load_event == WEBKIT_LOAD_FINISHED) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        save_history(uri);
        const gchar *title = webkit_web_view_get_title(web_view);
        if (!title || strlen(title) == 0) {
            title = "New Tab";
        }
        
        //truncate title if it's too long.
        const int MAX_TITLE_LENGTH = 20;
        char short_title[256] = {0}; //ensure the buffer is large enough.
        if (strlen(title) > MAX_TITLE_LENGTH) {
            strncpy(short_title, title, MAX_TITLE_LENGTH);
            short_title[MAX_TITLE_LENGTH] = '\0';
            strcat(short_title, "...");
            gtk_label_set_text(GTK_LABEL(tab->tab_label), short_title);
        } else {
            gtk_label_set_text(GTK_LABEL(tab->tab_label), title);
        }
    }
}


//decide-policy callback: block URLs if their host matches a blocked domain.
static gboolean decide_policy_callback(WebKitWebView *web_view,
                                         WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType type,
                                         gpointer user_data) {
    TabData *tab = (TabData *)user_data;
    AppData *app = tab->app;
    const gchar *uri = NULL;
    WebKitURIRequest *request = NULL;

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nav_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *nav_action = webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        request = webkit_navigation_action_get_request(nav_action);
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *resp_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        request = webkit_response_policy_decision_get_request(resp_decision);
    }

    if (request) {
        uri = webkit_uri_request_get_uri(request);
        if (uri) {
            GError *err = NULL;
            GUri *parsed_uri = g_uri_parse(uri, G_URI_FLAGS_NONE, &err);
            if (parsed_uri) {
                const gchar *host = g_uri_get_host(parsed_uri);
                if (host) {
                    /* Check against a built-in (hardcoded) list */
                    static const char *hardcoded_domains[] = {
                        "doubleclick.net",
                        "googlesyndication.com",
                        "adservice.google.com",
                        "ads.",
                        "adserver.",
                        NULL
                    };
                    for (int i = 0; hardcoded_domains[i] != NULL; i++) {
                        if (strstr(host, hardcoded_domains[i]) != NULL) {
                            g_print("Blocking ad (hardcoded): %s\n", uri);
                            webkit_policy_decision_ignore(decision);
                            g_uri_unref(parsed_uri);
                            return TRUE;
                        }
                    }
                    /* Check the trie-based block list */
                    if (app->blocked_domains_trie && search_domain(app->blocked_domains_trie, host)) {
                        g_print("Blocking ad (trie match): %s\n", uri);
                        webkit_policy_decision_ignore(decision);
                        g_uri_unref(parsed_uri);
                        return TRUE;
                    }
                }
                g_uri_unref(parsed_uri);
            } else {
                g_warning("Failed to parse URI (%s): %s", uri, err->message);
                g_error_free(err);
            }
        }
    }
    return FALSE; /* Use the default policy */
}

/* --- Navigation and UI Callbacks --- */

static void on_back_clicked(GtkButton *button, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    TabData *tab = get_active_tab(app);
    if (tab && webkit_web_view_can_go_back(tab->web_view))
        webkit_web_view_go_back(tab->web_view);
}

static void on_forward_clicked(GtkButton *button, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    TabData *tab = get_active_tab(app);
    if (tab && webkit_web_view_can_go_forward(tab->web_view))
        webkit_web_view_go_forward(tab->web_view);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    TabData *tab = get_active_tab(app);
    if (tab)
        webkit_web_view_reload(tab->web_view);
}

static void on_go_activated(GtkWidget *widget, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    const gchar *uri = gtk_entry_get_text(GTK_ENTRY(app->url_entry));
    TabData *tab = get_active_tab(app);
    if (tab && uri && *uri) {
        if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://")) {
            gchar *full_uri = g_strdup_printf("http://%s", uri);
            webkit_web_view_load_uri(tab->web_view, full_uri);
            g_free(full_uri);
        } else {
            webkit_web_view_load_uri(tab->web_view, uri);
        }
    }
}

static void on_new_tab_clicked(GtkButton *button, gpointer user_data) {
    AppData *app = (AppData *)user_data;
    create_new_tab(app, "");
}

//callback for the tab close button.
static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
    GtkWidget *container = GTK_WIDGET(user_data);
    //the container's parent is the notebook.
    GtkWidget *notebook = gtk_widget_get_parent(container);
    int page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), container);
    if (page_num != -1) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    }
    //free the associated TabData.
    TabData *tab = g_object_get_data(G_OBJECT(container), "tab-data");
    if (tab) {
        g_free(tab);
    }
}


/* --- Main Activate Function --- */

static void activate(GtkApplication *app_instance, gpointer user_data) {
    //allocate your AppData structure.
    AppData *app = g_malloc(sizeof(AppData));
    memset(app, 0, sizeof(AppData));

    //load the Glade file.
    GtkBuilder *builder = gtk_builder_new_from_file("kamakura_browser.glade");

    //get the main window from the builder.
    GtkWidget *window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    //associate the GtkApplication with the window.
    gtk_window_set_application(GTK_WINDOW(window), app_instance);

    //retrieve the widgets defined in Glade.
    GtkWidget *back_button    = GTK_WIDGET(gtk_builder_get_object(builder, "back_button"));
    GtkWidget *forward_button = GTK_WIDGET(gtk_builder_get_object(builder, "forward_button"));
    GtkWidget *refresh_button = GTK_WIDGET(gtk_builder_get_object(builder, "refresh_button"));
    app->url_entry            = GTK_WIDGET(gtk_builder_get_object(builder, "url_entry"));
    GtkWidget *go_button      = GTK_WIDGET(gtk_builder_get_object(builder, "go_button"));
    GtkWidget *new_tab_button = GTK_WIDGET(gtk_builder_get_object(builder, "new_tab_button"));
    app->notebook             = GTK_WIDGET(gtk_builder_get_object(builder, "notebook"));

    //initialize the global block list trie and load the block list.
    app->blocked_domains_trie = create_node();
    load_trie_block_list(app->blocked_domains_trie, "block.txt");

    //connect toolbar button signals.
    g_signal_connect(back_button,    "clicked", G_CALLBACK(on_back_clicked), app);
    g_signal_connect(forward_button, "clicked", G_CALLBACK(on_forward_clicked), app);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);
    g_signal_connect(go_button,      "clicked", G_CALLBACK(on_go_activated), app);
    g_signal_connect(app->url_entry, "activate", G_CALLBACK(on_go_activated), app);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), app);

    //redi
    create_new_tab(app, "http://www.example.com");

    //display the window.
    gtk_widget_show_all(window);

    //optionally, unref the builder since its objects are now part of the widget hierarchy.
    g_object_unref(builder);
}

/* --- Main Function --- */

int main(int argc, char **argv) {
    GtkApplication *app_instance;
    int status;

    app_instance = gtk_application_new("org.example.KamakuraBrowser", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app_instance, "activate", G_CALLBACK(activate), NULL);
    status = g_application_run(G_APPLICATION(app_instance), argc, argv);
    g_object_unref(app_instance);
    return status;
}
