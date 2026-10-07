/* Exercise real portal subscriptions and capture callbacks on a private bus. */
#include <glib.h>
#include <unistd.h>
#ifndef SCREENCAST_SOURCE
#define SCREENCAST_SOURCE "../plugins/linux-pipewire/screencast-portal.c"
#endif
#ifndef PORTAL_SOURCE
#define PORTAL_SOURCE "../plugins/linux-pipewire/portal.c"
#endif
#include SCREENCAST_SOURCE
#include PORTAL_SOURCE

struct request {
	char *method;
	char *path;
	char *session;
};
static GDBusConnection *service;
static GPtrArray *requests;
static unsigned starts;
static unsigned callbacks;

static const char xml[] =
	"<node><interface name='org.freedesktop.portal.ScreenCast'>"
	"<method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
	"<method name='SelectSources'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/>"
	"<arg type='o' direction='out'/></method>"
	"<method name='Start'><arg type='o' direction='in'/><arg type='s' direction='in'/>"
	"<arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
	"</interface></node>";

static void method_call(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
			const char *method, GVariant *params, GDBusMethodInvocation *invocation, void *data)
{
	(void)conn;
	(void)path;
	(void)iface;
	(void)data;
	g_autoptr(GVariant) options = g_variant_get_child_value(params, g_variant_n_children(params) - 1);
	const char *token = NULL;
	g_assert_true(g_variant_lookup(options, "handle_token", "&s", &token));
	char *sender_part = g_strdup(sender + 1);
	for (char *c = sender_part; *c; c++)
		if (*c == '.')
			*c = '_';
	struct request *request = g_new0(struct request, 1);
	request->method = g_strdup(method);
	request->path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender_part, token);
	if (!strcmp(method, "CreateSession")) {
		const char *session_token = NULL;
		g_assert_true(g_variant_lookup(options, "session_handle_token", "&s", &session_token));
		request->session =
			g_strdup_printf("/org/freedesktop/portal/desktop/session/%s/%s", sender_part, session_token);
	} else {
		g_autoptr(GVariant) handle = g_variant_get_child_value(params, 0);
		request->session = g_variant_dup_string(handle, NULL);
	}
	if (!strcmp(method, "Start"))
		starts++;
	g_ptr_array_add(requests, request);
	g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request->path));
	g_free(sender_part);
}

static void pump(void)
{
	gint64 deadline = g_get_monotonic_time() + 100000;
	do {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	} while (g_get_monotonic_time() < deadline);
}
static struct request *wait_request(unsigned index, const char *method)
{
	gint64 deadline = g_get_monotonic_time() + 2000000;
	while (requests->len <= index && g_get_monotonic_time() < deadline)
		pump();
	g_assert_cmpuint(requests->len, >, index);
	struct request *request = g_ptr_array_index(requests, index);
	g_assert_cmpstr(request->method, ==, method);
	return request;
}
static void emit_response(struct request *request, unsigned status, unsigned with_session)
{
	GVariantBuilder result;
	g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
	if (with_session)
		g_variant_builder_add(&result, "{sv}", "session_handle",
				      with_session == 2 ? g_variant_new_string(request->session)
							: g_variant_new_object_path(request->session));
	g_autoptr(GError) error = NULL;
	g_assert_true(g_dbus_connection_emit_signal(service, g_dbus_connection_get_unique_name(connection),
						    request->path, "org.freedesktop.portal.Request", "Response",
						    g_variant_new("(ua{sv})", status, &result), &error));
	g_assert_no_error(error);
	g_dbus_connection_flush_sync(service, NULL, NULL);
}
static void respond(struct request *request, unsigned status, unsigned with_session)
{
	emit_response(request, status, with_session);
	pump();
}
static struct screencast_portal_capture *new_capture(void)
{
	struct screencast_portal_capture *capture = bzalloc(sizeof(*capture));
	capture->capture_type = OBS_PORTAL_CAPTURE_TYPE_UNIFIED;
	capture->cursor_visible = true;
	g_assert_true(init_screencast_capture(capture));
	return capture;
}
static void normal_flow(unsigned handle_type)
{
	unsigned offset = requests->len;
	unsigned old_starts = starts;
	struct screencast_portal_capture *capture = new_capture();
	respond(wait_request(offset, "CreateSession"), 0, handle_type);
	respond(wait_request(offset + 1, "SelectSources"), 0, false);
	respond(wait_request(offset + 2, "Start"), 1, false);
	g_assert_cmpuint(starts, ==, old_starts + 1);
	screencast_portal_capture_destroy(capture);
	pump();
}
static void stale_selection(void)
{
	unsigned offset = requests->len;
	unsigned old_starts = starts;
	struct screencast_portal_capture *capture = new_capture();
	respond(wait_request(offset, "CreateSession"), 0, true);
	struct request *selection = wait_request(offset + 1, "SelectSources");
	GCancellable *old_cancel = g_object_ref(capture->cancellable);
	reload_session_cb(NULL, NULL, capture);
	wait_request(offset + 2, "CreateSession");
	/* Deliver the successful reply while the replacement has no session handle. */
	respond(selection, 0, false);
	g_assert_cmpuint(starts, ==, old_starts);
	g_assert_true(g_cancellable_is_cancelled(old_cancel));
	g_object_unref(old_cancel);
	respond(wait_request(offset + 2, "CreateSession"), 0, true);
	respond(wait_request(offset + 3, "SelectSources"), 0, false);
	respond(wait_request(offset + 4, "Start"), 1, false);
	g_assert_cmpuint(starts, ==, old_starts + 1);
	screencast_portal_capture_destroy(capture);
	pump();
}
static void repeated_reload(void)
{
	unsigned offset = requests->len;
	struct screencast_portal_capture *capture = new_capture();
	wait_request(offset, "CreateSession");
	for (unsigned i = 1; i <= 8; i++) {
		reload_session_cb(NULL, NULL, capture);
		wait_request(offset + i, "CreateSession");
	}
	for (unsigned i = 0; i < 8; i++)
		respond(wait_request(offset + i, "CreateSession"), 0, true);
	g_assert_null(capture->session_handle);
	respond(wait_request(offset + 8, "CreateSession"), 0, true);
	respond(wait_request(offset + 9, "SelectSources"), 1, false);
	screencast_portal_capture_destroy(capture);
	pump();
}
static void destroy_pending(void)
{
	unsigned offset = requests->len;
	struct screencast_portal_capture *capture = new_capture();
	struct request *creation = wait_request(offset, "CreateSession");
	screencast_portal_capture_destroy(capture);
	respond(creation, 0, true);
	g_assert_cmpuint(requests->len, ==, offset + 1);
}
static void destroy_queued(void)
{
	unsigned offset = requests->len;
	struct screencast_portal_capture *capture = new_capture();
	struct request *creation = wait_request(offset, "CreateSession");
	emit_response(creation, 0, true);
	/* Let the D-Bus worker receive it without dispatching the main context. */
	g_usleep(20000);
	screencast_portal_capture_destroy(capture);
	pump();
	g_assert_cmpuint(requests->len, ==, offset + 1);
}
static void cancel_in_callback(GVariant *params, void *data)
{
	(void)params;
	callbacks++;
	g_cancellable_cancel(data);
}
static void mark_finalized(void *data, GObject *object)
{
	(void)object;
	*(bool *)data = true;
}
static void callback_lifetime(void)
{
	bool finalized = false;
	GCancellable *cancel = g_cancellable_new();
	g_object_weak_ref(G_OBJECT(cancel), mark_finalized, &finalized);
	char *path = NULL;
	portal_create_request_path(&path, NULL);
	portal_signal_subscribe(path, cancel, cancel_in_callback, cancel);
	struct request request = {.path = path};
	respond(&request, 0, false);
	g_assert_cmpuint(callbacks, ==, 1);
	g_object_unref(cancel);
	pump();
	g_assert_true(finalized);
	bfree(path);
}
static void already_cancelled(void)
{
	GCancellable *cancel = g_cancellable_new();
	g_cancellable_cancel(cancel);
	char *path = NULL;
	portal_create_request_path(&path, NULL);
	portal_signal_subscribe(path, cancel, cancel_in_callback, cancel);
	struct request request = {.path = path};
	respond(&request, 0, false);
	g_assert_cmpuint(callbacks, ==, 1);
	g_object_unref(cancel);
	bfree(path);
}
static void invalid_handle(void)
{
	struct screencast_portal_capture capture = {0};
	GVariantBuilder result;
	g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
	g_autoptr(GVariant) params = g_variant_ref_sink(g_variant_new("(ua{sv})", 0, &result));
	on_create_session_response_received_cb(params, &capture);
	g_assert_null(capture.session_handle);
	g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&result, "{sv}", "session_handle", g_variant_new_uint32(42));
	g_autoptr(GVariant) bad_type = g_variant_ref_sink(g_variant_new("(ua{sv})", 0, &result));
	on_create_session_response_received_cb(bad_type, &capture);
	g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&result, "{sv}", "session_handle", g_variant_new_string("invalid/path"));
	g_autoptr(GVariant) bad_path = g_variant_ref_sink(g_variant_new("(ua{sv})", 0, &result));
	on_create_session_response_received_cb(bad_path, &capture);
	start(&capture);
	select_source(&capture);
	open_pipewire_remote(&capture);
}
int main(void)
{
	g_autoptr(GError) error = NULL;
	service = g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
							 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
								 G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
							 NULL, NULL, &error);
	g_assert_no_error(error);
	g_autoptr(GVariant) name_reply = g_dbus_connection_call_sync(
		service, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
		g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0), NULL, 0, -1, NULL, &error);
	g_assert_no_error(error);
	g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
	g_assert_no_error(error);
	static const GDBusInterfaceVTable vtable = {.method_call = method_call};
	guint registration = g_dbus_connection_register_object(service, "/org/freedesktop/portal/desktop",
							       info->interfaces[0], &vtable, NULL, NULL, &error);
	g_assert_no_error(error);
	screencast_proxy = g_dbus_proxy_new_sync(
		portal_get_dbus_connection(),
		G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
		"org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
		"org.freedesktop.portal.ScreenCast", NULL, &error);
	g_assert_no_error(error);
	g_dbus_proxy_set_cached_property(screencast_proxy, "version", g_variant_new_uint32(4));
	g_dbus_proxy_set_cached_property(screencast_proxy, "AvailableCursorModes", g_variant_new_uint32(3));
	requests = g_ptr_array_new();
	normal_flow(1);
	normal_flow(2);
	g_print("PASS normal portal flow with object-path and legacy string handles\n");
	stale_selection();
	g_print("PASS stale successful selection after reload\n");
	repeated_reload();
	g_print("PASS repeated reload and delayed CreateSession replies\n");
	destroy_pending();
	g_print("PASS source destruction with pending reply\n");
	destroy_queued();
	g_print("PASS source destruction with queued reply\n");
	callback_lifetime();
	g_print("PASS reentrant cancellation and reference release\n");
	already_cancelled();
	g_print("PASS subscription with cancelled token\n");
	invalid_handle();
	g_print("PASS missing, malformed and null session handles\n");
	for (unsigned i = 0; i < requests->len; i++) {
		struct request *request = g_ptr_array_index(requests, i);
		g_free(request->method);
		g_free(request->path);
		g_free(request->session);
		g_free(request);
	}
	g_ptr_array_unref(requests);
	g_dbus_connection_unregister_object(service, registration);
	g_clear_object(&screencast_proxy);
	g_clear_object(&service);
	g_clear_object(&connection);
	return 0;
}
