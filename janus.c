/*! \file   janus.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus core
 * \details Implementation of the gateway core. This code takes care of
 * the gateway initialization (command line/configuration) and setup,
 * and makes use of the available transport plugins (by default HTTP,
 * WebSockets, RabbitMQ, if compiled) and Janus protocol (a JSON-based
 * protocol) to interact with the applications, whether they're web based
 * or not. The core also takes care of bridging peers and plugins
 * accordingly, in terms of both messaging and real-time media transfer
 * via WebRTC.
 *
 * \ingroup core
 * \ref core
 */

#include <dlfcn.h>
#include <dirent.h>
#include <net/if.h>
#include <netdb.h>
#include <signal.h>
#include <getopt.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>

#include "janus.h"
#include "version.h"
#include "cmdline.h"
#include "config.h"
#include "apierror.h"
#include "debug.h"
#include "ip-utils.h"
#include "rtcp.h"
#include "auth.h"
#include "record.h"
#include "events.h"


#define JANUS_NAME				"Janus WebRTC Gateway"
#define JANUS_AUTHOR			"Meetecho s.r.l."
#define JANUS_SERVER_NAME		"MyJanusInstance"

#ifdef __MACH__
#define SHLIB_EXT "0.dylib"
#else
#define SHLIB_EXT ".so"
#endif


static janus_config *config = NULL;
static char *config_file = NULL;
static char *configs_folder = NULL;

static GHashTable *transports = NULL;
static GHashTable *transports_so = NULL;

static GHashTable *eventhandlers = NULL;
static GHashTable *eventhandlers_so = NULL;

static GHashTable *plugins = NULL;
static GHashTable *plugins_so = NULL;


/* Daemonization */
static gboolean daemonize = FALSE;
static int pipefd[2];


#ifdef REFCOUNT_DEBUG
/* Reference counters debugging */
GHashTable *counters = NULL;
janus_mutex counters_mutex;
#endif


/* API secrets */
static char *api_secret = NULL, *admin_api_secret = NULL;

/* JSON parameters */
static int janus_process_error_string(janus_request *request, uint64_t session_id, const char *transaction, gint error, gchar *error_string);

static struct janus_json_parameter incoming_request_parameters[] = {
	{"transaction", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"janus", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"id", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter attach_parameters[] = {
	{"plugin", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"opaque_id", JSON_STRING, 0},
};
static struct janus_json_parameter body_parameters[] = {
	{"body", JSON_OBJECT, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter jsep_parameters[] = {
	{"type", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"trickle", JANUS_JSON_BOOL, 0},
	{"sdp", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter add_token_parameters[] = {
	{"token", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"plugins", JSON_ARRAY, 0}
};
static struct janus_json_parameter token_parameters[] = {
	{"token", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter admin_parameters[] = {
	{"transaction", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"janus", JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter debug_parameters[] = {
	{"debug", JANUS_JSON_BOOL, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter timeout_parameters[] = {
	{"timeout", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter level_parameters[] = {
	{"level", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter timestamps_parameters[] = {
	{"timestamps", JANUS_JSON_BOOL, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter colors_parameters[] = {
	{"colors", JANUS_JSON_BOOL, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter mnq_parameters[] = {
	{"max_nack_queue", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter nmt_parameters[] = {
	{"no_media_timer", JSON_INTEGER, JANUS_JSON_PARAM_REQUIRED | JANUS_JSON_PARAM_POSITIVE}
};
static struct janus_json_parameter queryhandler_parameters[] = {
	{"handler", JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"request", JSON_OBJECT, 0}
};
static struct janus_json_parameter text2pcap_parameters[] = {
	{"folder", JSON_STRING, 0},
	{"filename", JSON_STRING, 0},
	{"truncate", JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};

/* Admin/Monitor helpers */
json_t *janus_admin_stream_summary(janus_ice_stream *stream);
json_t *janus_admin_component_summary(janus_ice_component *component);


/* IP addresses */
static gchar *local_ip = NULL;
gchar *janus_get_local_ip(void) {
	return local_ip;
}
static gchar *public_ip = NULL;
gchar *janus_get_public_ip(void) {
	/* Fallback to the local IP, if we have no public one */
	return public_ip ? public_ip : local_ip;
}
void janus_set_public_ip(const char *ip) {
	/* once set do not override */
	if(ip == NULL || public_ip != NULL)
		return;
	public_ip = g_strdup(ip);
}
static volatile gint stop = 0;
static gint stop_signal = 0;
gint janus_is_stopping(void) {
	return g_atomic_int_get(&stop);
}


/* Public instance name */
static gchar *server_name = NULL;

static json_t *janus_create_message(const char *status, uint64_t session_id, const char *transaction) {
	json_t *msg = json_object();
	json_object_set_new(msg, "janus", json_string(status));
	if(session_id > 0)
		json_object_set_new(msg, "session_id", json_integer(session_id));
	if(transaction != NULL)
		json_object_set_new(msg, "transaction", json_string(transaction));
	return msg;
}

/* The default timeout for sessions is 60 seconds: this means that, if
 * we don't get any activity (i.e., no request) on this session for more
 * than 60 seconds, then it's considered expired and we destroy it. That's
 * why we have a keep-alive method in the API. This can be overridden in
 * either janus.cfg or from the command line. Setting this to 0 will
 * disable the timeout mechanism, which is NOT suggested as it may risk
 * having orphaned sessions (sessions not controlled by any transport
 * and never freed). Besides, notice that if you make this shorter than
 * 30s, you'll have to update the timers in janus.js when the long
 * polling mechanism is used and shorten them as well, or you'll risk
 * incurring in unexpected timeouts (when HTTP is used in janus.js, the
 * long poll is used as a keepalive mechanism). */
#define DEFAULT_SESSION_TIMEOUT		60
static uint session_timeout = DEFAULT_SESSION_TIMEOUT;


/* Information */
static json_t *janus_info(const char *transaction) {
	/* Prepare a summary on the gateway */
	json_t *info = janus_create_message("server_info", 0, transaction);
	json_object_set_new(info, "name", json_string(JANUS_NAME));
	json_object_set_new(info, "version", json_integer(janus_version));
	json_object_set_new(info, "version_string", json_string(janus_version_string));
	json_object_set_new(info, "author", json_string(JANUS_AUTHOR));
	json_object_set_new(info, "commit-hash", json_string(janus_build_git_sha));
	json_object_set_new(info, "compile-time", json_string(janus_build_git_time));
	json_object_set_new(info, "log-to-stdout", janus_log_is_stdout_enabled() ? json_true() : json_false());
	json_object_set_new(info, "log-to-file", janus_log_is_logfile_enabled() ? json_true() : json_false());
	if(janus_log_is_logfile_enabled())
		json_object_set_new(info, "log-path", json_string(janus_log_get_logfile_path()));
#ifdef HAVE_SCTP
	json_object_set_new(info, "data_channels", json_true());
#else
	json_object_set_new(info, "data_channels", json_false());
#endif
	json_object_set_new(info, "session-timeout", json_integer(session_timeout));
	json_object_set_new(info, "server-name", json_string(server_name ? server_name : JANUS_SERVER_NAME));
	json_object_set_new(info, "local-ip", json_string(local_ip));
	if(public_ip != NULL)
		json_object_set_new(info, "public-ip", json_string(public_ip));
	json_object_set_new(info, "ipv6", janus_ice_is_ipv6_enabled() ? json_true() : json_false());
	json_object_set_new(info, "ice-lite", janus_ice_is_ice_lite_enabled() ? json_true() : json_false());
	json_object_set_new(info, "ice-tcp", janus_ice_is_ice_tcp_enabled() ? json_true() : json_false());
	json_object_set_new(info, "full-trickle", janus_ice_is_full_trickle_enabled() ? json_true() : json_false());
	json_object_set_new(info, "rfc-4588", janus_is_rfc4588_enabled() ? json_true() : json_false());
	if(janus_ice_get_stun_server() != NULL) {
		char server[255];
		g_snprintf(server, 255, "%s:%"SCNu16, janus_ice_get_stun_server(), janus_ice_get_stun_port());
		json_object_set_new(info, "stun-server", json_string(server));
	}
	if(janus_ice_get_turn_server() != NULL) {
		char server[255];
		g_snprintf(server, 255, "%s:%"SCNu16, janus_ice_get_turn_server(), janus_ice_get_turn_port());
		json_object_set_new(info, "turn-server", json_string(server));
	}
	json_object_set_new(info, "api_secret", api_secret ? json_true() : json_false());
	json_object_set_new(info, "auth_token", janus_auth_is_enabled() ? json_true() : json_false());
	json_object_set_new(info, "event_handlers", janus_events_is_enabled() ? json_true() : json_false());
	/* Available transports */
	json_t *t_data = json_object();
	if(transports && g_hash_table_size(transports) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, transports);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_transport *t = value;
			if(t == NULL) {
				continue;
			}
			json_t *transport = json_object();
			json_object_set_new(transport, "name", json_string(t->get_name()));
			json_object_set_new(transport, "author", json_string(t->get_author()));
			json_object_set_new(transport, "description", json_string(t->get_description()));
			json_object_set_new(transport, "version_string", json_string(t->get_version_string()));
			json_object_set_new(transport, "version", json_integer(t->get_version()));
			json_object_set_new(t_data, t->get_package(), transport);
		}
	}
	json_object_set_new(info, "transports", t_data);
	/* Available event handlers */
	json_t *e_data = json_object();
	if(eventhandlers && g_hash_table_size(eventhandlers) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, eventhandlers);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_eventhandler *e = value;
			if(e == NULL) {
				continue;
			}
			json_t *eventhandler = json_object();
			json_object_set_new(eventhandler, "name", json_string(e->get_name()));
			json_object_set_new(eventhandler, "author", json_string(e->get_author()));
			json_object_set_new(eventhandler, "description", json_string(e->get_description()));
			json_object_set_new(eventhandler, "version_string", json_string(e->get_version_string()));
			json_object_set_new(eventhandler, "version", json_integer(e->get_version()));
			json_object_set_new(e_data, e->get_package(), eventhandler);
		}
	}
	json_object_set_new(info, "events", e_data);
	/* Available plugins */
	json_t *p_data = json_object();
	if(plugins && g_hash_table_size(plugins) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, plugins);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_plugin *p = value;
			if(p == NULL) {
				continue;
			}
			json_t *plugin = json_object();
			json_object_set_new(plugin, "name", json_string(p->get_name()));
			json_object_set_new(plugin, "author", json_string(p->get_author()));
			json_object_set_new(plugin, "description", json_string(p->get_description()));
			json_object_set_new(plugin, "version_string", json_string(p->get_version_string()));
			json_object_set_new(plugin, "version", json_integer(p->get_version()));
			json_object_set_new(p_data, p->get_package(), plugin);
		}
	}
	json_object_set_new(info, "plugins", p_data);

	return info;
}


/* Logging */
int janus_log_level = LOG_INFO;
gboolean janus_log_timestamps = FALSE;
gboolean janus_log_colors = FALSE;
int lock_debug = 0;
#ifdef REFCOUNT_DEBUG
int refcount_debug = 1;
#else
int refcount_debug = 0;
#endif


/*! \brief Signal handler (just used to intercept CTRL+C and SIGTERM) */
static void janus_handle_signal(int signum) {
	stop_signal = signum;
	switch(g_atomic_int_get(&stop)) {
		case 0:
			JANUS_PRINT("Stopping gateway, please wait...\n");
			break;
		case 1:
			JANUS_PRINT("In a hurry? I'm trying to free resources cleanly, here!\n");
			break;
		default:
			JANUS_PRINT("Ok, leaving immediately...\n");
			break;
	}
	g_atomic_int_inc(&stop);
	if(g_atomic_int_get(&stop) > 2)
		exit(1);
}

/*! \brief Termination handler (atexit) */
static void janus_termination_handler(void) {
	/* Free the instance name, if provided */
	g_free(server_name);
	/* Remove the PID file if we created it */
	janus_pidfile_remove();
	/* Close the logger */
	janus_log_destroy();
	/* If we're daemonizing, we send an error code to the parent */
	if(daemonize) {
		int code = 1;
		ssize_t res = 0;
		do {
			res = write(pipefd[1], &code, sizeof(int));
		} while(res == -1 && errno == EINTR);
	}
}


/** @name Transport plugin callback interface
 * These are the callbacks implemented by the gateway core, as part of
 * the janus_transport_callbacks interface. Everything the transport
 * plugins send the gateway is handled here.
 */
///@{
void janus_transport_incoming_request(janus_transport *plugin, janus_transport_session *transport, void *request_id, gboolean admin, json_t *message, json_error_t *error);
void janus_transport_gone(janus_transport *plugin, janus_transport_session *transport);
gboolean janus_transport_is_api_secret_needed(janus_transport *plugin);
gboolean janus_transport_is_api_secret_valid(janus_transport *plugin, const char *apisecret);
gboolean janus_transport_is_auth_token_needed(janus_transport *plugin);
gboolean janus_transport_is_auth_token_valid(janus_transport *plugin, const char *token);
void janus_transport_notify_event(janus_transport *plugin, void *transport, json_t *event);

static janus_transport_callbacks janus_handler_transport =
	{
		.incoming_request = janus_transport_incoming_request,
		.transport_gone = janus_transport_gone,
		.is_api_secret_needed = janus_transport_is_api_secret_needed,
		.is_api_secret_valid = janus_transport_is_api_secret_valid,
		.is_auth_token_needed = janus_transport_is_auth_token_needed,
		.is_auth_token_valid = janus_transport_is_auth_token_valid,
		.events_is_enabled = janus_events_is_enabled,
		.notify_event = janus_transport_notify_event,
	};
static GAsyncQueue *requests = NULL;
static janus_request exit_message;
static GThreadPool *tasks = NULL;
void janus_transport_task(gpointer data, gpointer user_data);
///@}


/** @name Plugin callback interface
 * These are the callbacks implemented by the gateway core, as part of
 * the janus_callbacks interface. Everything the plugins send the
 * gateway is handled here.
 */
///@{
int janus_plugin_push_event(janus_plugin_session *plugin_session, janus_plugin *plugin, const char *transaction, json_t *message, json_t *jsep);
json_t *janus_plugin_handle_sdp(janus_plugin_session *plugin_session, janus_plugin *plugin, const char *sdp_type, const char *sdp, gboolean restart);
void janus_plugin_relay_rtp(janus_plugin_session *plugin_session, int video, char *buf, int len);
void janus_plugin_relay_rtcp(janus_plugin_session *plugin_session, int video, char *buf, int len);
void janus_plugin_relay_data(janus_plugin_session *plugin_session, char *buf, int len);
void janus_plugin_close_pc(janus_plugin_session *plugin_session);
void janus_plugin_end_session(janus_plugin_session *plugin_session);
void janus_plugin_notify_event(janus_plugin *plugin, janus_plugin_session *plugin_session, json_t *event);
static janus_callbacks janus_handler_plugin =
	{
		.push_event = janus_plugin_push_event,
		.relay_rtp = janus_plugin_relay_rtp,
		.relay_rtcp = janus_plugin_relay_rtcp,
		.relay_data = janus_plugin_relay_data,
		.close_pc = janus_plugin_close_pc,
		.end_session = janus_plugin_end_session,
		.events_is_enabled = janus_events_is_enabled,
		.notify_event = janus_plugin_notify_event,
	}; 
///@}


/* Gateway Sessions */
static janus_mutex sessions_mutex;
static GHashTable *sessions = NULL;
static GMainContext *sessions_watchdog_context = NULL;


static void janus_ice_handle_dereference(janus_ice_handle *handle) {
	if(handle)
		janus_refcount_decrease(&handle->ref);
}

static void janus_session_free(const janus_refcount *session_ref) {
	janus_session *session = janus_refcount_containerof(session_ref, janus_session, ref);
	/* This session can be destroyed, free all the resources */
	if(session->ice_handles != NULL) {
		g_hash_table_destroy(session->ice_handles);
		session->ice_handles = NULL;
	}
	if(session->source != NULL) {
		janus_request_destroy(session->source);
		session->source = NULL;
	}
	g_free(session);
}

static gboolean janus_check_sessions(gpointer user_data) {
	if(session_timeout < 1)		/* Session timeouts are disabled */
		return G_SOURCE_CONTINUE;
	janus_mutex_lock(&sessions_mutex);
	if(sessions && g_hash_table_size(sessions) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, sessions);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_session *session = (janus_session *) value;
			if (!session || g_atomic_int_get(&session->destroyed)) {
				continue;
			}
			gint64 now = janus_get_monotonic_time();
			if (now - session->last_activity >= (gint64)session_timeout * G_USEC_PER_SEC &&
					!g_atomic_int_compare_and_exchange(&session->timeout, 0, 1)) {
				JANUS_LOG(LOG_INFO, "Timeout expired for session %"SCNu64"...\n", session->session_id);
				/* Mark the session as over, we'll deal with it later */
				janus_session_handles_clear(session);
				/* Notify the transport */
				if(session->source) {
					json_t *event = janus_create_message("timeout", session->session_id, NULL);
					/* Send this to the transport client and notify the session's over */
					session->source->transport->send_message(session->source->instance, NULL, FALSE, event);
					session->source->transport->session_over(session->source->instance, session->session_id, TRUE);
				}
				/* Notify event handlers as well */
				if(janus_events_is_enabled())
					janus_events_notify_handlers(JANUS_EVENT_TYPE_SESSION, session->session_id, "timeout", NULL);

				/* FIXME Is this safe? apparently it causes hash table errors on the console */
				g_hash_table_iter_remove(&iter);

				janus_session_destroy(session);
			}
		}
	}
	janus_mutex_unlock(&sessions_mutex);

	return G_SOURCE_CONTINUE;
}

static gpointer janus_sessions_watchdog(gpointer user_data) {
	GMainLoop *loop = (GMainLoop *) user_data;
	GMainContext *watchdog_context = g_main_loop_get_context(loop);
	GSource *timeout_source;

	timeout_source = g_timeout_source_new_seconds(2);
	g_source_set_callback(timeout_source, janus_check_sessions, watchdog_context, NULL);
	g_source_attach(timeout_source, watchdog_context);
	g_source_unref(timeout_source);

	JANUS_LOG(LOG_INFO, "Sessions watchdog started\n");

	g_main_loop_run(loop);

	JANUS_LOG(LOG_INFO, "Sessions watchdog stopped\n");

	return NULL;
}


janus_session *janus_session_create(guint64 session_id) {
	janus_session *session = NULL;
	if(session_id == 0) {
		while(session_id == 0) {
			session_id = janus_random_uint64();
			session = janus_session_find(session_id);
			if(session != NULL) {
				/* Session ID already taken, try another one */
				janus_refcount_decrease(&session->ref);
				session_id = 0;
			}
		}
	}
	session = (janus_session *)g_malloc(sizeof(janus_session));
	JANUS_LOG(LOG_INFO, "Creating new session: %"SCNu64"; %p\n", session_id, session);
	session->session_id = session_id;
	janus_refcount_init(&session->ref, janus_session_free);
	session->source = NULL;
	g_atomic_int_set(&session->destroyed, 0);
	g_atomic_int_set(&session->timeout, 0);
	session->last_activity = janus_get_monotonic_time();
	session->ice_handles = NULL;
	janus_mutex_init(&session->mutex);
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, janus_uint64_dup(session->session_id), session);
	janus_mutex_unlock(&sessions_mutex);
	return session;
}

janus_session *janus_session_find(guint64 session_id) {
	janus_mutex_lock(&sessions_mutex);
	janus_session *session = g_hash_table_lookup(sessions, &session_id);
	if(session != NULL) {
		/* A successful find automatically increases the reference counter:
		 * it's up to the caller to decrease it again when done */
		janus_refcount_increase(&session->ref);
	}
	janus_mutex_unlock(&sessions_mutex);
	return session;
}

void janus_session_notify_event(janus_session *session, json_t *event) {
	if(session != NULL && !g_atomic_int_get(&session->destroyed) && session->source != NULL && session->source->transport != NULL) {
		/* Send this to the transport client */
		JANUS_LOG(LOG_HUGE, "Sending event to %s (%p)\n", session->source->transport->get_package(), session->source->instance);
		session->source->transport->send_message(session->source->instance, NULL, FALSE, event);
	} else {
		/* No transport, free the event */
		json_decref(event);
	}
}


/* Destroys a session but does not remove it from the sessions hash table. */
gint janus_session_destroy(janus_session *session) {
	guint64 session_id = session->session_id;
	JANUS_LOG(LOG_INFO, "Destroying session %"SCNu64"; %p\n", session_id, session);
	if(!g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		return 0;
	janus_session_handles_clear(session);
	/* The session will actually be destroyed when the counter gets to 0 */
	janus_refcount_decrease(&session->ref);

	return 0;
}

janus_ice_handle *janus_session_handles_find(janus_session *session, guint64 handle_id) {
	if(session == NULL)
		return NULL;
	janus_mutex_lock(&session->mutex);
	janus_ice_handle *handle = session->ice_handles ? g_hash_table_lookup(session->ice_handles, &handle_id) : NULL;
	if(handle != NULL) {
		/* A successful find automatically increases the reference counter:
		 * it's up to the caller to decrease it again when done */
		janus_refcount_increase(&handle->ref);
	}
	janus_mutex_unlock(&session->mutex);
	return handle;
}

void janus_session_handles_insert(janus_session *session, janus_ice_handle *handle) {
	janus_mutex_lock(&session->mutex);
	if(session->ice_handles == NULL)
		session->ice_handles = g_hash_table_new_full(g_int64_hash, g_int64_equal, (GDestroyNotify)g_free, (GDestroyNotify)janus_ice_handle_dereference);
	janus_refcount_increase(&handle->ref);
	g_hash_table_insert(session->ice_handles, janus_uint64_dup(handle->handle_id), handle);
	janus_mutex_unlock(&session->mutex);
}

gint janus_session_handles_remove(janus_session *session, janus_ice_handle *handle) {
	janus_mutex_lock(&session->mutex);
	gint error = janus_ice_handle_destroy(session, handle);
	g_hash_table_remove(session->ice_handles, &handle->handle_id);
	janus_mutex_unlock(&session->mutex);
	return error;
}

void janus_session_handles_clear(janus_session *session) {
	janus_mutex_lock(&session->mutex);
	if(session->ice_handles != NULL && g_hash_table_size(session->ice_handles) > 0) {
		GHashTableIter iter;
		gpointer value;
		/* Remove all handles */
		g_hash_table_iter_init(&iter, session->ice_handles);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_ice_handle *handle = value;
			if(!handle)
				continue;
			janus_ice_handle_destroy(session, handle);
			g_hash_table_iter_remove(&iter);
		}
	}
	janus_mutex_unlock(&session->mutex);
}

json_t *janus_session_handles_list_json(janus_session *session) {
	json_t *list = json_array();
	janus_mutex_lock(&session->mutex);
	if(session->ice_handles != NULL && g_hash_table_size(session->ice_handles) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, session->ice_handles);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_ice_handle *handle = value;
			if(!handle)
				continue;
			json_array_append_new(list, json_integer(handle->handle_id));
		}
	}
	janus_mutex_unlock(&session->mutex);
	return list;
}

/* Requests management */
janus_request *janus_request_new(janus_transport *transport, janus_transport_session *instance, void *request_id, gboolean admin, json_t *message) {
	janus_request *request = g_malloc(sizeof(janus_request));
	request->transport = transport;
	request->instance = instance;
	janus_refcount_increase(&instance->ref);
	request->request_id = request_id;
	request->admin = admin;
	request->message = message;
	return request;
}

void janus_request_destroy(janus_request *request) {
	if(request == NULL || request == &exit_message)
		return;
	request->transport = NULL;
	janus_refcount_decrease(&request->instance->ref);
	request->instance = NULL;
	request->request_id = NULL;
	if(request->message)
		json_decref(request->message);
	request->message = NULL;
	g_free(request);
}

static int janus_request_check_secret(janus_request *request, guint64 session_id, const gchar *transaction_text) {
	gboolean secret_authorized = FALSE, token_authorized = FALSE;
	if(api_secret == NULL && !janus_auth_is_enabled()) {
		/* Nothing to check */
		secret_authorized = TRUE;
		token_authorized = TRUE;
	} else {
		json_t *root = request->message;
		if(api_secret != NULL) {
			/* There's an API secret, check that the client provided it */
			json_t *secret = json_object_get(root, "apisecret");
			if(secret && json_is_string(secret) && janus_strcmp_const_time(json_string_value(secret), api_secret)) {
				secret_authorized = TRUE;
			}
		}
		if(janus_auth_is_enabled()) {
			/* The token based authentication mechanism is enabled, check that the client provided it */
			json_t *token = json_object_get(root, "token");
			if(token && json_is_string(token) && janus_auth_check_token(json_string_value(token))) {
				token_authorized = TRUE;
			}
		}
		/* We consider a request authorized if either the proper API secret or a valid token has been provided */
		if(!secret_authorized && !token_authorized) {
			return janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNAUTHORIZED, NULL);
		}
	}
	return 0;
}

static void janus_request_ice_handle_answer(janus_ice_handle *handle, int audio, int video, int data, char *jsep_sdp) {
	/* We got our answer */
	janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
	/* Any pending trickles? */
	if(handle->pending_trickles) {
		JANUS_LOG(LOG_VERB, "[%"SCNu64"]   -- Processing %d pending trickle candidates\n", handle->handle_id, g_list_length(handle->pending_trickles));
		GList *temp = NULL;
		while(handle->pending_trickles) {
			temp = g_list_first(handle->pending_trickles);
			handle->pending_trickles = g_list_remove_link(handle->pending_trickles, temp);
			janus_ice_trickle *trickle = (janus_ice_trickle *)temp->data;
			g_list_free(temp);
			if(trickle == NULL)
				continue;
			if((janus_get_monotonic_time() - trickle->received) > 45*G_USEC_PER_SEC) {
				/* FIXME Candidate is too old, discard it */
				janus_ice_trickle_destroy(trickle);
				/* FIXME We should report that */
				continue;
			}
			json_t *candidate = trickle->candidate;
			if(candidate == NULL) {
				janus_ice_trickle_destroy(trickle);
				continue;
			}
			if(json_is_object(candidate)) {
				/* We got a single candidate */
				int error = 0;
				const char *error_string = NULL;
				if((error = janus_ice_trickle_parse(handle, candidate, &error_string)) != 0) {
					/* FIXME We should report the error parsing the trickle candidate */
				}
			} else if(json_is_array(candidate)) {
				/* We got multiple candidates in an array */
				JANUS_LOG(LOG_VERB, "[%"SCNu64"] Got multiple candidates (%zu)\n", handle->handle_id, json_array_size(candidate));
				if(json_array_size(candidate) > 0) {
					/* Handle remote candidates */
					size_t i = 0;
					for(i=0; i<json_array_size(candidate); i++) {
						json_t *c = json_array_get(candidate, i);
						/* FIXME We don't care if any trickle fails to parse */
						janus_ice_trickle_parse(handle, c, NULL);
					}
				}
			}
			/* Done, free candidate */
			janus_ice_trickle_destroy(trickle);
		}
	}
	/* This was an answer, check if it's time to start ICE */
	if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE) &&
		!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALL_TRICKLES)) {
		JANUS_LOG(LOG_VERB, "[%"SCNu64"]   -- ICE Trickling is supported by the browser, waiting for remote candidates...\n", handle->handle_id);
		janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_START);
	} else {
		JANUS_LOG(LOG_VERB, "[%"SCNu64"] Done! Sending connectivity checks...\n", handle->handle_id);
		janus_ice_setup_remote_candidates(handle, handle->stream_id, 1);
	}
}

int janus_process_incoming_request(janus_request *request) {
	int ret = -1;
	if(request == NULL) {
		JANUS_LOG(LOG_ERR, "Missing request or payload to process, giving up...\n");
		return ret;
	}
	int error_code = 0;
	char error_cause[100];
	json_t *root = request->message;
	/* Ok, let's start with the ids */
	guint64 session_id = 0, handle_id = 0;
	json_t *s = json_object_get(root, "session_id");
	if(s && json_is_integer(s))
		session_id = json_integer_value(s);
	json_t *h = json_object_get(root, "handle_id");
	if(h && json_is_integer(h))
		handle_id = json_integer_value(h);

	janus_session *session = NULL;
	janus_ice_handle *handle = NULL;

	/* Get transaction and message request */
	JANUS_VALIDATE_JSON_OBJECT(root, incoming_request_parameters,
		error_code, error_cause, FALSE,
		JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
	if(error_code != 0) {
		ret = janus_process_error_string(request, session_id, NULL, error_code, error_cause);
		goto jsondone;
	}
	json_t *transaction = json_object_get(root, "transaction");
	const gchar *transaction_text = json_string_value(transaction);
	json_t *message = json_object_get(root, "janus");
	const gchar *message_text = json_string_value(message);

	if(session_id == 0 && handle_id == 0) {
		/* Can only be a 'Create new session', a 'Get info' or a 'Ping/Pong' request */
		if(!strcasecmp(message_text, "info")) {
			ret = janus_process_success(request, janus_info(transaction_text));
			goto jsondone;
		}
		if(!strcasecmp(message_text, "ping")) {
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("pong", 0, transaction_text);
			ret = janus_process_success(request, reply);
			goto jsondone;
		}
		if(strcasecmp(message_text, "create")) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		/* Any secret/token to check? */
		ret = janus_request_check_secret(request, session_id, transaction_text);
		if(ret != 0)
			goto jsondone;
		session_id = 0;
		json_t *id = json_object_get(root, "id");
		if(id != NULL) {
			/* The application provided the session ID to use */
			session_id = json_integer_value(id);
			if(session_id > 0 && (session = janus_session_find(session_id)) != NULL) {
				/* Session ID already taken */
				janus_refcount_decrease(&session->ref);
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_CONFLICT, "Session ID already in use");
				goto jsondone;
			}
		}
		/* Handle it */
		session = janus_session_create(session_id);
		if(session == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Memory error");
			goto jsondone;
		}
		session_id = session->session_id;
		/* We increase the counter as this request is using the session */
		janus_refcount_increase(&session->ref);
		/* Take note of the request source that originated this session (HTTP, WebSockets, RabbitMQ?) */
		session->source = janus_request_new(request->transport, request->instance, NULL, FALSE, NULL);
		/* Notify the source that a new session has been created */
		request->transport->session_created(request->instance, session->session_id);
		/* Notify event handlers */
		if(janus_events_is_enabled()) {
			/* Session created, add info on the transport that originated it */
			json_t *transport = json_object();
			json_object_set_new(transport, "transport", json_string(session->source->transport->get_package()));
			char id[32];
			memset(id, 0, sizeof(id));
			g_snprintf(id, sizeof(id), "%p", session->source->instance);
			json_object_set_new(transport, "id", json_string(id));
			janus_events_notify_handlers(JANUS_EVENT_TYPE_SESSION, session_id, "created", transport);
		}
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", 0, transaction_text);
		json_t *data = json_object();
		json_object_set_new(data, "id", json_integer(session_id));
		json_object_set_new(reply, "data", data);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
		goto jsondone;
	}
	if(session_id < 1) {
		JANUS_LOG(LOG_ERR, "Invalid session\n");
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}
	if(h && handle_id < 1) {
		JANUS_LOG(LOG_ERR, "Invalid handle\n");
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}

	/* Go on with the processing */
	ret = janus_request_check_secret(request, session_id, transaction_text);
	if(ret != 0)
		goto jsondone;

	/* If we got here, make sure we have a session (and/or a handle) */
	session = janus_session_find(session_id);
	if(!session) {
		JANUS_LOG(LOG_ERR, "Couldn't find any session %"SCNu64"...\n", session_id);
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, "No such session %"SCNu64"", session_id);
		goto jsondone;
	}
	/* Update the last activity timer */
	session->last_activity = janus_get_monotonic_time();
	handle = NULL;
	if(handle_id > 0) {
		handle = janus_session_handles_find(session, handle_id);
		if(!handle) {
			JANUS_LOG(LOG_ERR, "Couldn't find any handle %"SCNu64" in session %"SCNu64"...\n", handle_id, session_id);
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_HANDLE_NOT_FOUND, "No such handle %"SCNu64" in session %"SCNu64"", handle_id, session_id);
			goto jsondone;
		}
	}

	/* What is this? */
	if(!strcasecmp(message_text, "keepalive")) {
		/* Just a keep-alive message, reply with an ack */
		JANUS_LOG(LOG_VERB, "Got a keep-alive on session %"SCNu64"\n", session_id);
		json_t *reply = janus_create_message("ack", session_id, transaction_text);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
	} else if(!strcasecmp(message_text, "attach")) {
		if(handle != NULL) {
			/* Attach is a session-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		JANUS_VALIDATE_JSON_OBJECT(root, attach_parameters,
			error_code, error_cause, FALSE,
			JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
		if(error_code != 0) {
			ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
			goto jsondone;
		}
		json_t *plugin = json_object_get(root, "plugin");
		const gchar *plugin_text = json_string_value(plugin);
		janus_plugin *plugin_t = janus_plugin_find(plugin_text);
		if(plugin_t == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_NOT_FOUND, "No such plugin '%s'", plugin_text);
			goto jsondone;
		}
		/* If the auth token mechanism is enabled, we should check if this token can access this plugin */
		if(janus_auth_is_enabled()) {
			json_t *token = json_object_get(root, "token");
			if(token != NULL) {
				const char *token_value = json_string_value(token);
				if(token_value && !janus_auth_check_plugin(token_value, plugin_t)) {
					JANUS_LOG(LOG_ERR, "Token '%s' can't access plugin '%s'\n", token_value, plugin_text);
					ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNAUTHORIZED_PLUGIN, "Provided token can't access plugin '%s'", plugin_text);
					goto jsondone;
				}
			}
		}
		json_t *opaque = json_object_get(root, "opaque_id");
		const char *opaque_id = opaque ? json_string_value(opaque) : NULL;
		/* Create handle */
		handle = janus_ice_handle_create(session, opaque_id);
		if(handle == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Memory error");
			janus_mutex_unlock(&session->mutex);
			goto jsondone;
		}
		handle_id = handle->handle_id;
		/* We increase the counter as this request is using the handle */
		janus_refcount_increase(&handle->ref);
		/* Attach to the plugin */
		int error = 0;
		if((error = janus_ice_handle_attach_plugin(session, handle, plugin_t)) != 0) {
			/* TODO Make error struct to pass verbose information */
			janus_session_handles_remove(session, handle);
			JANUS_LOG(LOG_ERR, "Couldn't attach to plugin '%s', error '%d'\n", plugin_text, error);
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_ATTACH, "Couldn't attach to plugin: error '%d'", error);
			goto jsondone;
		}
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		json_t *data = json_object();
		json_object_set_new(data, "id", json_integer(handle_id));
		json_object_set_new(reply, "data", data);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
	} else if(!strcasecmp(message_text, "destroy")) {
		if(handle != NULL) {
			/* Query is a session-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		janus_mutex_lock(&sessions_mutex);
		g_hash_table_remove(sessions, &session->session_id);
		janus_mutex_unlock(&sessions_mutex);
		/* Notify the source that the session has been destroyed */
		if(session->source && session->source->transport) {
			session->source->transport->session_over(session->source->instance, session->session_id, FALSE);
		}
		/* Schedule the session for deletion */
		janus_session_destroy(session);

		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
		/* Notify event handlers as well */
		if(janus_events_is_enabled())
			janus_events_notify_handlers(JANUS_EVENT_TYPE_SESSION, session_id, "destroyed", NULL);
	} else if(!strcasecmp(message_text, "detach")) {
		if(handle == NULL) {
			/* Query is an handle-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "No plugin to detach from");
			goto jsondone;
		}
		int error = janus_session_handles_remove(session, handle);
		if(error != 0) {
			/* TODO Make error struct to pass verbose information */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "Couldn't detach from plugin: error '%d'", error);
			/* TODO Delete handle instance */
			goto jsondone;
		}
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
	} else if(!strcasecmp(message_text, "hangup")) {
		if(handle == NULL) {
			/* Query is an handle-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_DETACH, "No plugin attached");
			goto jsondone;
		}
		janus_ice_webrtc_hangup(handle, "Janus API");
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
	} else if(!strcasecmp(message_text, "message")) {
		if(handle == NULL) {
			/* Query is an handle-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "No plugin to handle this message");
			goto jsondone;
		}
		janus_plugin *plugin_t = (janus_plugin *)handle->app;
		JANUS_LOG(LOG_VERB, "[%"SCNu64"] There's a message for %s\n", handle->handle_id, plugin_t->get_name());
		JANUS_VALIDATE_JSON_OBJECT(root, body_parameters,
			error_code, error_cause, FALSE,
			JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
		if(error_code != 0) {
			ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
			goto jsondone;
		}
		json_t *body = json_object_get(root, "body");
		/* Is there an SDP attached? */
		json_t *jsep = json_object_get(root, "jsep");
		char *jsep_type = NULL;
		char *jsep_sdp = NULL, *jsep_sdp_stripped = NULL;
		gboolean renegotiation = FALSE;
		if(jsep != NULL) {
			if(!json_is_object(jsep)) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_JSON_OBJECT, "Invalid jsep object");
				goto jsondone;
			}
			JANUS_VALIDATE_JSON_OBJECT_FORMAT("JSEP error: missing mandatory element (%s)",
				"JSEP error: invalid element type (%s should be %s)",
				jsep, jsep_parameters, error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *type = json_object_get(jsep, "type");
			jsep_type = g_strdup(json_string_value(type));
			type = NULL;
			gboolean do_trickle = TRUE;
			json_t *jsep_trickle = json_object_get(jsep, "trickle");
			do_trickle = jsep_trickle ? json_is_true(jsep_trickle) : TRUE;
			/* Are we still cleaning up from a previous media session? */
			if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
				JANUS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", handle->handle_id);
				gint64 waited = 0;
				while(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
					g_usleep(100000);
					waited += 100000;
					if(waited >= 3*G_USEC_PER_SEC) {
						JANUS_LOG(LOG_VERB, "[%"SCNu64"]   -- Waited 3 seconds, that's enough!\n", handle->handle_id);
						ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_WEBRTC_STATE, "Still cleaning a previous session");
						goto jsondone;
					}
				}
			}
			/* Check the JSEP type */
			janus_mutex_lock(&handle->mutex);
			int offer = 0;
			if(!strcasecmp(jsep_type, "offer")) {
				offer = 1;
				janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER);
				janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
			} else if(!strcasecmp(jsep_type, "answer")) {
				janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
				offer = 0;
			} else {
				/* TODO Handle other message types as well */
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_JSEP_UNKNOWN_TYPE, "JSEP error: unknown message type '%s'", jsep_type);
				g_free(jsep_type);
				janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				janus_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			json_t *sdp = json_object_get(jsep, "sdp");
			jsep_sdp = (char *)json_string_value(sdp);
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Remote SDP:\n%s", handle->handle_id, jsep_sdp);
			/* Is this valid SDP? */
			char error_str[512];
			int audio = 0, video = 0, data = 0;
			janus_sdp *parsed_sdp = janus_sdp_preparse(jsep_sdp, error_str, sizeof(error_str), &audio, &video, &data);
			if(parsed_sdp == NULL) {
				/* Invalid SDP */
				ret = janus_process_error_string(request, session_id, transaction_text, JANUS_ERROR_JSEP_INVALID_SDP, error_str);
				g_free(jsep_type);
				janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				janus_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			/* Notify event handlers */
			if(janus_events_is_enabled()) {
				janus_events_notify_handlers(JANUS_EVENT_TYPE_JSEP,
					session_id, handle_id, handle->opaque_id, "remote", jsep_type, jsep_sdp);
			}
			/* FIXME We're only handling single audio/video lines for now... */
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Audio %s been negotiated, Video %s been negotiated, SCTP/DataChannels %s been negotiated\n",
			                    handle->handle_id,
			                    audio ? "has" : "has NOT",
			                    video ? "has" : "has NOT",
			                    data ? "have" : "have NOT");
			if(audio > 1) {
				JANUS_LOG(LOG_WARN, "[%"SCNu64"] More than one audio line? only going to negotiate one...\n", handle->handle_id);
			}
			if(video > 1) {
				JANUS_LOG(LOG_WARN, "[%"SCNu64"] More than one video line? only going to negotiate one...\n", handle->handle_id);
			}
			if(data > 1) {
				JANUS_LOG(LOG_WARN, "[%"SCNu64"] More than one data line? only going to negotiate one...\n", handle->handle_id);
			}
#ifndef HAVE_SCTP
			if(data) {
				JANUS_LOG(LOG_WARN, "[%"SCNu64"]   -- DataChannels have been negotiated, but support for them has not been compiled...\n", handle->handle_id);
			}
#endif
			/* Check if it's a new session, or an update... */
			if(!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_READY)
					|| janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				/* New session */
				if(offer) {
					/* Setup ICE locally (we received an offer) */
					if(janus_ice_setup_local(handle, offer, audio, video, data, do_trickle) < 0) {
						JANUS_LOG(LOG_ERR, "Error setting ICE locally\n");
						janus_sdp_destroy(parsed_sdp);
						g_free(jsep_type);
						janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
						ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Error setting ICE locally");
						janus_mutex_unlock(&handle->mutex);
						goto jsondone;
					}
				} else {
					/* Make sure we're waiting for an ANSWER in the first place */
					if(!handle->agent) {
						JANUS_LOG(LOG_ERR, "Unexpected ANSWER (did we offer?)\n");
						janus_sdp_destroy(parsed_sdp);
						g_free(jsep_type);
						janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
						ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNEXPECTED_ANSWER, "Unexpected ANSWER (did we offer?)");
						janus_mutex_unlock(&handle->mutex);
						goto jsondone;
					}
				}
				if(janus_sdp_process(handle, parsed_sdp, FALSE) < 0) {
					JANUS_LOG(LOG_ERR, "Error processing SDP\n");
					janus_sdp_destroy(parsed_sdp);
					g_free(jsep_type);
					janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
					ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_JSEP_INVALID_SDP, "Error processing SDP");
					janus_mutex_unlock(&handle->mutex);
					goto jsondone;
				}
				if(!offer) {
					/* Set remote candidates now (we received an answer) */
					janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE);
					janus_request_ice_handle_answer(handle, audio, video, data, jsep_sdp);
				} else {
					/* Check if transport wide CC is supported */
					int transport_wide_cc_ext_id = janus_rtp_header_extension_get_id(jsep_sdp, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC);
					handle->stream->do_transport_wide_cc = TRUE;
					handle->stream->transport_wide_cc_ext_id = transport_wide_cc_ext_id;
				}
			} else {
				/* FIXME This is a renegotiation: we can currently only handle simple changes in media
				 * direction and ICE restarts: anything more complex than that will result in an error */
				JANUS_LOG(LOG_INFO, "[%"SCNu64"] Negotiation update, checking what changed...\n", handle->handle_id);
				if(janus_sdp_process(handle, parsed_sdp, TRUE) < 0) {
					JANUS_LOG(LOG_ERR, "Error processing SDP\n");
					janus_sdp_destroy(parsed_sdp);
					g_free(jsep_type);
					janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
					ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNEXPECTED_ANSWER, "Error processing SDP");
					janus_mutex_unlock(&handle->mutex);
					goto jsondone;
				}
				renegotiation = TRUE;
				if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ICE_RESTART)) {
					JANUS_LOG(LOG_INFO, "[%"SCNu64"] Restarting ICE...\n", handle->handle_id);
					/* Update remote credentials for ICE */
					if(handle->stream) {
						nice_agent_set_remote_credentials(handle->agent, handle->stream->stream_id,
							handle->stream->ruser, handle->stream->rpass);
					}
					/* FIXME We only need to do that for offers: if it's an answer, we did that already */
					if(offer) {
						janus_ice_restart(handle);
					} else {
						janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ICE_RESTART);
					}
					/* If we're full-trickling, we'll need to resend the candidates later */
					if(janus_ice_is_full_trickle_enabled()) {
						janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RESEND_TRICKLES);
					}
				}
#ifdef HAVE_SCTP
				if(!offer) {
					/* Were datachannels just added? */
					if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_DATA_CHANNELS)) {
						janus_ice_stream *stream = handle->stream;
						if(stream != NULL && stream->component != NULL
								&& stream->component->dtls != NULL && stream->component->dtls->sctp == NULL) {
							/* Create SCTP association as well */
							JANUS_LOG(LOG_WARN, "[%"SCNu64"] Creating datachannels...\n", handle->handle_id);
							janus_dtls_srtp_create_sctp(stream->component->dtls);
						}
					}
				}
#endif
			}
			char *tmp = handle->remote_sdp;
			handle->remote_sdp = g_strdup(jsep_sdp);
			g_free(tmp);
			janus_mutex_unlock(&handle->mutex);
			/* Anonymize SDP */
			if(janus_sdp_anonymize(parsed_sdp) < 0) {
				/* Invalid SDP */
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_JSEP_INVALID_SDP, "JSEP error: invalid SDP");
				janus_sdp_destroy(parsed_sdp);
				g_free(jsep_type);
				janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
				goto jsondone;
			}
			jsep_sdp_stripped = janus_sdp_write(parsed_sdp);
			janus_sdp_destroy(parsed_sdp);
			sdp = NULL;
			janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
		}

		/* Make sure the app handle is still valid */
		if(handle->app == NULL || handle->app_handle == NULL || !janus_plugin_session_is_alive(handle->app_handle)) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "No plugin to handle this message");
			g_free(jsep_type);
			g_free(jsep_sdp_stripped);
			janus_flags_clear(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
			goto jsondone;
		}

		/* Send the message to the plugin (which must eventually free transaction_text and unref the two objects, body and jsep) */
		json_incref(body);
		json_t *body_jsep = NULL;
		if(jsep_sdp_stripped) {
			body_jsep = json_pack("{ssss}", "type", jsep_type, "sdp", jsep_sdp_stripped);
			/* Check if VP8 simulcasting is enabled */
			if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_VIDEO)) {
				if(handle->stream && handle->stream->video_ssrc_peer[1]) {
					json_t *simulcast = json_object();
					json_object_set(simulcast, "ssrc-0", json_integer(handle->stream->video_ssrc_peer[0]));
					json_object_set(simulcast, "ssrc-1", json_integer(handle->stream->video_ssrc_peer[1]));
					if(handle->stream->video_ssrc_peer[2])
						json_object_set(simulcast, "ssrc-2", json_integer(handle->stream->video_ssrc_peer[2]));
					json_object_set(body_jsep, "simulcast", simulcast);
				}
			}
			/* Check if this is a renegotiation or update */
			if(renegotiation)
				json_object_set(body_jsep, "update", json_true());
		}
		janus_plugin_result *result = plugin_t->handle_message(handle->app_handle,
			g_strdup((char *)transaction_text), body, body_jsep);
		g_free(jsep_type);
		g_free(jsep_sdp_stripped);
		if(result == NULL) {
			/* Something went horribly wrong! */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "Plugin didn't give a result");
			goto jsondone;
		}
		if(result->type == JANUS_PLUGIN_OK) {
			/* The plugin gave a result already (synchronous request/response) */
			if(result->content == NULL || !json_is_object(result->content)) {
				/* Missing content, or not a JSON object */
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE,
					result->content == NULL ?
						"Plugin didn't provide any content for this synchronous response" :
						"Plugin returned an invalid JSON response");
				janus_plugin_result_destroy(result);
				goto jsondone;
			}
			/* Reference the content, as destroying the result instance will decref it */
			json_incref(result->content);
			/* Prepare JSON response */
			json_t *reply = janus_create_message("success", session->session_id, transaction_text);
			json_object_set_new(reply, "sender", json_integer(handle->handle_id));
			json_t *plugin_data = json_object();
			json_object_set_new(plugin_data, "plugin", json_string(plugin_t->get_package()));
			json_object_set_new(plugin_data, "data", result->content);
			json_object_set_new(reply, "plugindata", plugin_data);
			/* Send the success reply */
			ret = janus_process_success(request, reply);
		} else if(result->type == JANUS_PLUGIN_OK_WAIT) {
			/* The plugin received the request but didn't process it yet, send an ack (asynchronous notifications may follow) */
			json_t *reply = janus_create_message("ack", session_id, transaction_text);
			if(result->text)
				json_object_set_new(reply, "hint", json_string(result->text));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
		} else {
			/* Something went horribly wrong! */
			ret = janus_process_error_string(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE,
				(char *)(result->text ? result->text : "Plugin returned a severe (unknown) error"));
			janus_plugin_result_destroy(result);
			goto jsondone;
		}
		janus_plugin_result_destroy(result);
	} else if(!strcasecmp(message_text, "trickle")) {
		if(handle == NULL) {
			/* Trickle is an handle-level command */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		if(handle->app == NULL || handle->app_handle == NULL || !janus_plugin_session_is_alive(handle->app_handle)) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_MESSAGE, "No plugin to handle this trickle candidate");
			goto jsondone;
		}
		json_t *candidate = json_object_get(root, "candidate");
		json_t *candidates = json_object_get(root, "candidates");
		if(candidate == NULL && candidates == NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_MISSING_MANDATORY_ELEMENT, "Missing mandatory element (candidate|candidates)");
			goto jsondone;
		}
		if(candidate != NULL && candidates != NULL) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_JSON, "Can't have both candidate and candidates");
			goto jsondone;
		}
		if(janus_flags_is_set(&handle->webrtc_flags,JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
			JANUS_LOG(LOG_ERR, "[%"SCNu64"] Received a trickle, but still cleaning a previous session\n", handle->handle_id);
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_WEBRTC_STATE, "Still cleaning a previous session");
			goto jsondone;
		}
		janus_mutex_lock(&handle->mutex);
		if(!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE)) {
			/* It looks like this peer supports Trickle, after all */
			JANUS_LOG(LOG_VERB, "Handle %"SCNu64" supports trickle even if it didn't negotiate it...\n", handle->handle_id);
			janus_flags_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE);
		}
		/* Is there any stream ready? this trickle may get here before the SDP it relates to */
		if(handle->stream == NULL) {
			JANUS_LOG(LOG_WARN, "[%"SCNu64"] No stream, queueing this trickle as it got here before the SDP...\n", handle->handle_id);
			/* Enqueue this trickle candidate(s), we'll process this later */
			janus_ice_trickle *early_trickle = janus_ice_trickle_new(transaction_text, candidate ? candidate : candidates);
			handle->pending_trickles = g_list_append(handle->pending_trickles, early_trickle);
			/* Send the ack right away, an event will tell the application if the candidate(s) failed */
			goto trickledone;
		}
		/* Is the ICE stack ready already? */
		if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER) ||
				!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER) ||
				!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER)) {
			const char *cause = NULL;
			if(janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER))
				cause = "processing the offer";
			else if(!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER))
				cause = "waiting for the answer";
			else if(!janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER))
				cause = "waiting for the offer";
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Still %s, queueing this trickle to wait until we're done there...\n",
				handle->handle_id, cause);
			/* Enqueue this trickle candidate(s), we'll process this later */
			janus_ice_trickle *early_trickle = janus_ice_trickle_new(transaction_text, candidate ? candidate : candidates);
			handle->pending_trickles = g_list_append(handle->pending_trickles, early_trickle);
			/* Send the ack right away, an event will tell the application if the candidate(s) failed */
			goto trickledone;
		}
		if(candidate != NULL) {
			/* We got a single candidate */
			int error = 0;
			const char *error_string = NULL;
			if((error = janus_ice_trickle_parse(handle, candidate, &error_string)) != 0) {
				ret = janus_process_error(request, session_id, transaction_text, error, "%s", error_string);
				janus_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
		} else {
			/* We got multiple candidates in an array */
			if(!json_is_array(candidates)) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "candidates is not an array");
				janus_mutex_unlock(&handle->mutex);
				goto jsondone;
			}
			JANUS_LOG(LOG_VERB, "Got multiple candidates (%zu)\n", json_array_size(candidates));
			if(json_array_size(candidates) > 0) {
				/* Handle remote candidates */
				size_t i = 0;
				for(i=0; i<json_array_size(candidates); i++) {
					json_t *c = json_array_get(candidates, i);
					/* FIXME We don't care if any trickle fails to parse */
					janus_ice_trickle_parse(handle, c, NULL);
				}
			}
		}

trickledone:
		janus_mutex_unlock(&handle->mutex);
		/* We reply right away, not to block the web server... */
		json_t *reply = janus_create_message("ack", session_id, transaction_text);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
	} else {
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN_REQUEST, "Unknown request '%s'", message_text);
	}

jsondone:
	/* Done processing */
	if(handle != NULL)
		janus_refcount_decrease(&handle->ref);
	if(session != NULL)
		janus_refcount_decrease(&session->ref);
	return ret;
}

static json_t *janus_json_token_plugin_array(const char *token_value) {
	json_t *plugins_list = json_array();
	GList *plugins = janus_auth_list_plugins(token_value);
	if(plugins != NULL) {
		GList *tmp = plugins;
		while(tmp) {
			janus_plugin *p = (janus_plugin *)tmp->data;
			if(p != NULL)
				json_array_append_new(plugins_list, json_string(p->get_package()));
			tmp = tmp->next;
		}
		g_list_free(plugins);
		plugins = NULL;
	}
	return plugins_list;
}

static json_t *janus_json_list_token_plugins(const char *token_value, const gchar *transaction_text) {
	json_t *plugins_list = janus_json_token_plugin_array(token_value);
	/* Prepare JSON reply */
	json_t *reply = janus_create_message("success", 0, transaction_text);
	json_t *data = json_object();
	json_object_set_new(data, "plugins", plugins_list);
	json_object_set_new(reply, "data", data);
	return reply;
}

static int janus_request_allow_token(janus_request *request, guint64 session_id, const gchar *transaction_text, gboolean allow, gboolean add) {
	/* Allow/disallow a valid token valid to access a plugin */
	int ret = -1;
	int error_code = 0;
	char error_cause[100];
	json_t *root = request->message;
	if(!janus_auth_is_enabled()) {
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Token based authentication disabled");
		goto jsondone;
	}
	JANUS_VALIDATE_JSON_OBJECT(root, add_token_parameters,
		error_code, error_cause, FALSE,
		JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
	/* Any plugin this token should be limited to? */
	json_t *allowed = json_object_get(root, "plugins");
	if(error_code == 0 && !add && (!allowed || json_array_size(allowed) == 0)) {
		error_code = JANUS_ERROR_INVALID_ELEMENT_TYPE;
		g_strlcpy(error_cause, "Invalid element type (plugins should be a non-empty array)", sizeof(error_cause));
	}
	if(error_code != 0) {
		ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
		goto jsondone;
	}
	json_t *token = json_object_get(root, "token");
	const char *token_value = json_string_value(token);
	if(add) {
		/* First of all, add the new token */
		if(!janus_auth_add_token(token_value)) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Error adding token");
			goto jsondone;
		}
	} else {
		/* Check if the token is valid, first */
		if(!janus_auth_check_token(token_value)) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_TOKEN_NOT_FOUND, "Token %s not found", token_value);
			goto jsondone;
		}
	}
	if(allowed && json_array_size(allowed) > 0) {
		/* Specify which plugins this token has access to */
		size_t i = 0;
		gboolean ok = TRUE;
		for(i=0; i<json_array_size(allowed); i++) {
			json_t *p = json_array_get(allowed, i);
			if(!p || !json_is_string(p)) {
				/* FIXME Should we fail here? */
				if(add){
					JANUS_LOG(LOG_WARN, "Invalid plugin passed to the new token request, skipping...\n");
					continue;
				} else {
					JANUS_LOG(LOG_ERR, "Invalid plugin passed to the new token request...\n");
					ok = FALSE;
					break;
				}
			}
			const gchar *plugin_text = json_string_value(p);
			janus_plugin *plugin_t = janus_plugin_find(plugin_text);
			if(plugin_t == NULL) {
				/* FIXME Should we fail here? */
				if(add) {
					JANUS_LOG(LOG_WARN, "No such plugin '%s' passed to the new token request, skipping...\n", plugin_text);
					continue;
				} else {
					JANUS_LOG(LOG_ERR, "No such plugin '%s' passed to the new token request...\n", plugin_text);
					ok = FALSE;
				}
				break;
			}
		}
		if(!ok) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (some of the provided plugins are invalid)");
			goto jsondone;
		}
		/* Take care of the plugins access limitations */
		i = 0;
		for(i=0; i<json_array_size(allowed); i++) {
			json_t *p = json_array_get(allowed, i);
			const gchar *plugin_text = json_string_value(p);
			janus_plugin *plugin_t = janus_plugin_find(plugin_text);
			if(!(allow ? janus_auth_allow_plugin(token_value, plugin_t) : janus_auth_disallow_plugin(token_value, plugin_t))) {
				/* FIXME Should we notify individual failures? */
				JANUS_LOG(LOG_WARN, "Error allowing access to '%s' to the new token, bad things may happen...\n", plugin_text);
			}
		}
	} else {
		/* No plugin limitation specified, allow all plugins */
		if(plugins && g_hash_table_size(plugins) > 0) {
			GHashTableIter iter;
			gpointer value;
			g_hash_table_iter_init(&iter, plugins);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				janus_plugin *plugin_t = value;
				if(plugin_t == NULL)
					continue;
				if(!janus_auth_allow_plugin(token_value, plugin_t)) {
					JANUS_LOG(LOG_WARN, "Error allowing access to '%s' to the new token, bad things may happen...\n", plugin_t->get_package());
				}
			}
		}
	}
	/* Get the list of plugins this new token can now access */
	json_t *reply = janus_json_list_token_plugins(token_value, transaction_text);
	/* Send the success reply */
	ret = janus_process_success(request, reply);
jsondone:
	return ret;
}

/* Admin/monitor WebServer requests handler */
int janus_process_incoming_admin_request(janus_request *request) {
	int ret = -1;
	int error_code = 0;
	char error_cause[100];
	if(request == NULL) {
		JANUS_LOG(LOG_ERR, "Missing request or payload to process, giving up...\n");
		return ret;
	}
	json_t *root = request->message;
	/* Ok, let's start with the ids */
	guint64 session_id = 0, handle_id = 0;
	json_t *s = json_object_get(root, "session_id");
	if(s && json_is_integer(s))
		session_id = json_integer_value(s);
	json_t *h = json_object_get(root, "handle_id");
	if(h && json_is_integer(h))
		handle_id = json_integer_value(h);

	janus_session *session = NULL;
	janus_ice_handle *handle = NULL;

	/* Get transaction and message request */
	JANUS_VALIDATE_JSON_OBJECT(root, admin_parameters,
		error_code, error_cause, FALSE,
		JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
	if(error_code != 0) {
		ret = janus_process_error_string(request, session_id, NULL, error_code, error_cause);
		goto jsondone;
	}
	json_t *transaction = json_object_get(root, "transaction");
	const gchar *transaction_text = json_string_value(transaction);
	json_t *message = json_object_get(root, "janus");
	const gchar *message_text = json_string_value(message);

	if(session_id == 0 && handle_id == 0) {
		/* Can only be a 'Get all sessions' or some general setting manipulation request */
		if(!strcasecmp(message_text, "info")) {
			/* The generic info request */
			ret = janus_process_success(request, janus_info(transaction_text));
			goto jsondone;
		}
		if(admin_api_secret != NULL) {
			/* There's an admin/monitor secret, check that the client provided it */
			json_t *secret = json_object_get(root, "admin_secret");
			if(!secret || !json_is_string(secret) || !janus_strcmp_const_time(json_string_value(secret), admin_api_secret)) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNAUTHORIZED, NULL);
				goto jsondone;
			}
		}
		if(!strcasecmp(message_text, "get_status")) {
			/* Return some info on the settings (mostly debug-related, at the moment) */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_t *status = json_object();
			json_object_set_new(status, "token_auth", janus_auth_is_enabled() ? json_true() : json_false());
			json_object_set_new(status, "session_timeout", json_integer(session_timeout));
			json_object_set_new(status, "log_level", json_integer(janus_log_level));
			json_object_set_new(status, "log_timestamps", janus_log_timestamps ? json_true() : json_false());
			json_object_set_new(status, "log_colors", janus_log_colors ? json_true() : json_false());
			json_object_set_new(status, "locking_debug", lock_debug ? json_true() : json_false());
			json_object_set_new(status, "refcount_debug", refcount_debug ? json_true() : json_false());
			json_object_set_new(status, "libnice_debug", janus_ice_is_ice_debugging_enabled() ? json_true() : json_false());
			json_object_set_new(status, "max_nack_queue", json_integer(janus_get_max_nack_queue()));
			json_object_set_new(status, "no_media_timer", json_integer(janus_get_no_media_timer()));
			json_object_set_new(reply, "status", status);
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_session_timeout")) {
			/* Change the session timeout value */
			JANUS_VALIDATE_JSON_OBJECT(root, timeout_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *timeout = json_object_get(root, "timeout");
			int timeout_num = json_integer_value(timeout);
			if(timeout_num < 0) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (timeout should be a positive integer)");
				goto jsondone;
			}
			session_timeout = timeout_num;
			/* Prepare JSON reply */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			json_object_set_new(reply, "timeout", json_integer(session_timeout));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_log_level")) {
			/* Change the debug logging level */
			JANUS_VALIDATE_JSON_OBJECT(root, level_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *level = json_object_get(root, "level");
			int level_num = json_integer_value(level);
			if(level_num < LOG_NONE || level_num > LOG_MAX) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (level should be between %d and %d)", LOG_NONE, LOG_MAX);
				goto jsondone;
			}
			janus_log_level = level_num;
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "level", json_integer(janus_log_level));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_locking_debug")) {
			/* Enable/disable the locking debug (would show a message on the console for every lock attempt) */
			JANUS_VALIDATE_JSON_OBJECT(root, debug_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *debug = json_object_get(root, "debug");
			lock_debug = json_is_true(debug);
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "locking_debug", lock_debug ? json_true() : json_false());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_refcount_debug")) {
			/* Enable/disable the reference counter debug (would show a message on the console for every increase/decrease) */
			JANUS_VALIDATE_JSON_OBJECT(root, debug_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *debug = json_object_get(root, "debug");
			if(json_is_true(debug)) {
				refcount_debug = TRUE;
			} else {
				refcount_debug = FALSE;
			}
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "refcount_debug", refcount_debug ? json_true() : json_false());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_log_timestamps")) {
			/* Enable/disable the log timestamps */
			JANUS_VALIDATE_JSON_OBJECT(root, timestamps_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *timestamps = json_object_get(root, "timestamps");
			janus_log_timestamps = json_is_true(timestamps);
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "log_timestamps", janus_log_timestamps ? json_true() : json_false());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_log_colors")) {
			/* Enable/disable the log colors */
			JANUS_VALIDATE_JSON_OBJECT(root, colors_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *colors = json_object_get(root, "colors");
			janus_log_colors = json_is_true(colors);
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "log_colors", janus_log_colors ? json_true() : json_false());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_libnice_debug")) {
			/* Enable/disable the libnice debugging (http://nice.freedesktop.org/libnice/libnice-Debug-messages.html) */
			JANUS_VALIDATE_JSON_OBJECT(root, debug_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *debug = json_object_get(root, "debug");
			if(json_is_true(debug)) {
				janus_ice_debugging_enable();
			} else {
				janus_ice_debugging_disable();
			}
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "libnice_debug", janus_ice_is_ice_debugging_enabled() ? json_true() : json_false());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_max_nack_queue")) {
			/* Change the current value for the max NACK queue */
			JANUS_VALIDATE_JSON_OBJECT(root, mnq_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *mnq = json_object_get(root, "max_nack_queue");
			int mnq_num = json_integer_value(mnq);
			if(mnq_num < 0 || (mnq_num > 0 && mnq_num < 200)) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_ELEMENT_TYPE, "Invalid element type (max_nack_queue, if provided, should be greater than 200)");
				goto jsondone;
			}
			janus_set_max_nack_queue(mnq_num);
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "max_nack_queue", json_integer(janus_get_max_nack_queue()));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "set_no_media_timer")) {
			/* Change the current value for the no-media timer */
			JANUS_VALIDATE_JSON_OBJECT(root, nmt_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *nmt = json_object_get(root, "no_media_timer");
			int nmt_num = json_integer_value(nmt);
			janus_set_no_media_timer(nmt_num);
			/* Prepare JSON reply */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			json_object_set_new(reply, "no_media_timer", json_integer(janus_get_no_media_timer()));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "query_eventhandler")) {
			/* Contact an event handler and expect a response */
			JANUS_VALIDATE_JSON_OBJECT(root, queryhandler_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *handler = json_object_get(root, "handler");
			const char *handler_value = json_string_value(handler);
			janus_eventhandler *evh = g_hash_table_lookup(eventhandlers, handler_value);
			if(evh == NULL) {
				/* No such handler... */
				g_snprintf(error_cause, sizeof(error_cause), "%s", "Invalid event handler");
				ret = janus_process_error_string(request, session_id, transaction_text, JANUS_ERROR_PLUGIN_NOT_FOUND, error_cause);
				goto jsondone;
			}
			if(evh->handle_request == NULL) {
				/* Handler doesn't implement the hook... */
				g_snprintf(error_cause, sizeof(error_cause), "%s", "Event handler doesn't support queries");
				ret = janus_process_error_string(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, error_cause);
				goto jsondone;
			}
			json_t *query = json_object_get(root, "request");
			json_t *response = evh->handle_request(query);
			/* Prepare JSON reply */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			json_object_set_new(reply, "response", response ? response : json_object());
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "list_sessions")) {
			/* List sessions */
			session_id = 0;
			json_t *list = json_array();
			if(sessions != NULL && g_hash_table_size(sessions) > 0) {
				janus_mutex_lock(&sessions_mutex);
				GHashTableIter iter;
				gpointer value;
				g_hash_table_iter_init(&iter, sessions);
				while (g_hash_table_iter_next(&iter, NULL, &value)) {
					janus_session *session = value;
					if(session == NULL) {
						continue;
					}
					json_array_append_new(list, json_integer(session->session_id));
				}
				janus_mutex_unlock(&sessions_mutex);
			}
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_object_set_new(reply, "sessions", list);
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "add_token")) {
			/* Add a token valid for authentication */
			ret = janus_request_allow_token(request, session_id, transaction_text, TRUE, TRUE);
			goto jsondone;
		} else if(!strcasecmp(message_text, "list_tokens")) {
			/* List all the valid tokens */
			if(!janus_auth_is_enabled()) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Token based authentication disabled");
				goto jsondone;
			}
			json_t *tokens_list = json_array();
			GList *list = janus_auth_list_tokens();
			if(list != NULL) {
				GList *tmp = list;
				while(tmp) {
					char *token = (char *)tmp->data;
					if(token != NULL) {
						json_t *plugins_list = janus_json_token_plugin_array(token);
						if(json_array_size(plugins_list) > 0) {
							json_t *t = json_object();
							json_object_set_new(t, "token", json_string(token));
							json_object_set_new(t, "allowed_plugins", plugins_list);
							json_array_append_new(tokens_list, t);
						}
						else
							json_decref(plugins_list);
						tmp->data = NULL;
						g_free(token);
					}
					tmp = tmp->next;
				}
				g_list_free(list);
			}
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			json_t *data = json_object();
			json_object_set_new(data, "tokens", tokens_list);
			json_object_set_new(reply, "data", data);
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "allow_token")) {
			/* Allow a valid token valid to access a plugin */
			ret = janus_request_allow_token(request, session_id, transaction_text, TRUE, FALSE);
			goto jsondone;
		} else if(!strcasecmp(message_text, "disallow_token")) {
			/* Disallow a valid token valid from accessing a plugin */
			ret = janus_request_allow_token(request, session_id, transaction_text, FALSE, FALSE);
			goto jsondone;
		} else if(!strcasecmp(message_text, "remove_token")) {
			/* Invalidate a token for authentication purposes */
			if(!janus_auth_is_enabled()) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Token based authentication disabled");
				goto jsondone;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, token_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			json_t *token = json_object_get(root, "token");
			const char *token_value = json_string_value(token);
			if(!janus_auth_remove_token(token_value)) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Error removing token");
				goto jsondone;
			}
			/* Prepare JSON reply */
			json_t *reply = janus_create_message("success", 0, transaction_text);
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else {
			/* No message we know of */
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
	}
	if(session_id < 1) {
		JANUS_LOG(LOG_ERR, "Invalid session\n");
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}
	if(h && handle_id < 1) {
		JANUS_LOG(LOG_ERR, "Invalid handle\n");
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, NULL);
		goto jsondone;
	}

	/* Go on with the processing */
	if(admin_api_secret != NULL) {
		/* There's an API secret, check that the client provided it */
		json_t *secret = json_object_get(root, "admin_secret");
		if(!secret || !json_is_string(secret) || !janus_strcmp_const_time(json_string_value(secret), admin_api_secret)) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNAUTHORIZED, NULL);
			goto jsondone;
		}
	}

	/* If we got here, make sure we have a session (and/or a handle) */
	session = janus_session_find(session_id);
	if(!session) {
		JANUS_LOG(LOG_ERR, "Couldn't find any session %"SCNu64"...\n", session_id);
		ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_SESSION_NOT_FOUND, "No such session %"SCNu64"", session_id);
		goto jsondone;
	}
	handle = NULL;
	if(handle_id > 0) {
		handle = janus_session_handles_find(session, handle_id);
		if(!handle) {
			JANUS_LOG(LOG_ERR, "Couldn't find any handle %"SCNu64" in session %"SCNu64"...\n", handle_id, session_id);
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_HANDLE_NOT_FOUND, "No such handle %"SCNu64" in session %"SCNu64"", handle_id, session_id);
			goto jsondone;
		}
	}

	/* What is this? */
	if(handle == NULL) {
		/* Session-related */
		if(strcasecmp(message_text, "list_handles")) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		/* List handles */
		json_t *list = janus_session_handles_list_json(session);
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		json_object_set_new(reply, "handles", list);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
		goto jsondone;
	} else {
		/* Handle-related */
		if(!strcasecmp(message_text, "start_text2pcap")) {
			/* Start dumping RTP and RTCP packets to a text2pcap file */
			JANUS_VALIDATE_JSON_OBJECT(root, text2pcap_parameters,
				error_code, error_cause, FALSE,
				JANUS_ERROR_MISSING_MANDATORY_ELEMENT, JANUS_ERROR_INVALID_ELEMENT_TYPE);
			if(error_code != 0) {
				ret = janus_process_error_string(request, session_id, transaction_text, error_code, error_cause);
				goto jsondone;
			}
			const char *folder = json_string_value(json_object_get(root, "folder"));
			const char *filename = json_string_value(json_object_get(root, "filename"));
			int truncate = json_integer_value(json_object_get(root, "truncate"));
			if(handle->text2pcap != NULL) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "text2pcap already started");
				goto jsondone;
			}
			handle->text2pcap = janus_text2pcap_create(folder, filename, truncate);
			if(handle->text2pcap == NULL) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "Error starting text2pcap dump");
				goto jsondone;
			}
			g_atomic_int_set(&handle->dump_packets, 1);
			/* Prepare JSON reply */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		} else if(!strcasecmp(message_text, "stop_text2pcap")) {
			/* Stop dumping RTP and RTCP packets to a text2pcap file */
			if(handle->text2pcap == NULL) {
				ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_UNKNOWN, "text2pcap not started");
				goto jsondone;
			}
			if(g_atomic_int_compare_and_exchange(&handle->dump_packets, 1, 0)) {
				janus_text2pcap_close(handle->text2pcap);
				g_clear_pointer(&handle->text2pcap, janus_text2pcap_free);
			}
			/* Prepare JSON reply */
			json_t *reply = json_object();
			json_object_set_new(reply, "janus", json_string("success"));
			json_object_set_new(reply, "transaction", json_string(transaction_text));
			/* Send the success reply */
			ret = janus_process_success(request, reply);
			goto jsondone;
		}
		/* If this is not a request to start/stop debugging to text2pcap, it must be a handle_info */
		if(strcasecmp(message_text, "handle_info")) {
			ret = janus_process_error(request, session_id, transaction_text, JANUS_ERROR_INVALID_REQUEST_PATH, "Unhandled request '%s' at this path", message_text);
			goto jsondone;
		}
		/* Prepare info */
		janus_mutex_lock(&handle->mutex);
		json_t *info = json_object();
		json_object_set_new(info, "session_id", json_integer(session_id));
		json_object_set_new(info, "session_last_activity", json_integer(session->last_activity));
		if(session->source && session->source->transport)
			json_object_set_new(info, "session_transport", json_string(session->source->transport->get_package()));
		json_object_set_new(info, "handle_id", json_integer(handle_id));
		if(handle->opaque_id)
			json_object_set_new(info, "opaque_id", json_string(handle->opaque_id));
		json_object_set_new(info, "created", json_integer(handle->created));
		json_object_set_new(info, "send_thread_created", g_atomic_int_get(&handle->send_thread_created) ? json_true() : json_false());
		json_object_set_new(info, "current_time", json_integer(janus_get_monotonic_time()));
		if(handle->app && handle->app_handle && janus_plugin_session_is_alive(handle->app_handle)) {
			janus_plugin *plugin = (janus_plugin *)handle->app;
			json_object_set_new(info, "plugin", json_string(plugin->get_package()));
			if(plugin->query_session) {
				/* FIXME This check will NOT work with legacy plugins that were compiled BEFORE the method was specified in plugin.h */
				json_t *query = plugin->query_session(handle->app_handle);
				if(query != NULL) {
					/* Make sure this is a JSON object */
					if(!json_is_object(query)) {
						JANUS_LOG(LOG_WARN, "Ignoring invalid query response from the plugin (not an object)\n");
						json_decref(query);
					} else {
						json_object_set_new(info, "plugin_specific", query);
					}
					query = NULL;
				}
			}
		}
		json_t *flags = json_object();
		json_object_set_new(flags, "got-offer", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER) ? json_true() : json_false());
		json_object_set_new(flags, "got-answer", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER) ? json_true() : json_false());
		json_object_set_new(flags, "processing-offer", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER) ? json_true() : json_false());
		json_object_set_new(flags, "starting", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_START) ? json_true() : json_false());
		json_object_set_new(flags, "ice-restart", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ICE_RESTART) ? json_true() : json_false());
		json_object_set_new(flags, "ready", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_READY) ? json_true() : json_false());
		json_object_set_new(flags, "stopped", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP) ? json_true() : json_false());
		json_object_set_new(flags, "alert", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT) ? json_true() : json_false());
		json_object_set_new(flags, "trickle", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE) ? json_true() : json_false());
		json_object_set_new(flags, "all-trickles", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALL_TRICKLES) ? json_true() : json_false());
		json_object_set_new(flags, "resend-trickles", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RESEND_TRICKLES) ? json_true() : json_false());
		json_object_set_new(flags, "trickle-synced", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_TRICKLE_SYNCED) ? json_true() : json_false());
		json_object_set_new(flags, "data-channels", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_DATA_CHANNELS) ? json_true() : json_false());
		json_object_set_new(flags, "has-audio", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_AUDIO) ? json_true() : json_false());
		json_object_set_new(flags, "has-video", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_VIDEO) ? json_true() : json_false());
		json_object_set_new(flags, "rfc4588-rtx", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RFC4588_RTX) ? json_true() : json_false());
		json_object_set_new(flags, "cleaning", janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING) ? json_true() : json_false());
		json_object_set_new(info, "flags", flags);
		if(handle->agent) {
			json_object_set_new(info, "agent-created", json_integer(handle->agent_created));
			json_object_set_new(info, "ice-mode", json_string(janus_ice_is_ice_lite_enabled() ? "lite" : "full"));
			json_object_set_new(info, "ice-role", json_string(handle->controlling ? "controlling" : "controlled"));
		}
		json_t *sdps = json_object();
		if(handle->rtp_profile)
			json_object_set_new(sdps, "profile", json_string(handle->rtp_profile));
		if(handle->local_sdp)
			json_object_set_new(sdps, "local", json_string(handle->local_sdp));
		if(handle->remote_sdp)
			json_object_set_new(sdps, "remote", json_string(handle->remote_sdp));
		json_object_set_new(info, "sdps", sdps);
		if(handle->pending_trickles)
			json_object_set_new(info, "pending-trickles", json_integer(g_list_length(handle->pending_trickles)));
		if(handle->queued_packets)
			json_object_set_new(info, "queued-packets", json_integer(g_async_queue_length(handle->queued_packets)));
		if(g_atomic_int_get(&handle->dump_packets)) {
			json_object_set_new(info, "dump-to-text2pcap", json_true());
			if(handle->text2pcap && handle->text2pcap->filename)
			json_object_set_new(info, "text2pcap-file", json_string(handle->text2pcap->filename));
		}
		json_t *streams = json_array();
		if(handle->stream) {
			json_t *s = janus_admin_stream_summary(handle->stream);
			if(s)
				json_array_append_new(streams, s);
		}
		json_object_set_new(info, "streams", streams);
		janus_mutex_unlock(&handle->mutex);
		/* Prepare JSON reply */
		json_t *reply = janus_create_message("success", session_id, transaction_text);
		json_object_set_new(reply, "handle_id", json_integer(handle_id));
		json_object_set_new(reply, "info", info);
		/* Send the success reply */
		ret = janus_process_success(request, reply);
		goto jsondone;
	}

jsondone:
	/* Done processing */
	if(handle != NULL)
		janus_refcount_decrease(&handle->ref);
	if(session != NULL)
		janus_refcount_decrease(&session->ref);
	return ret;
}

int janus_process_success(janus_request *request, json_t *payload)
{
	if(!request || !payload)
		return -1;
	/* Pass to the right transport plugin */
	JANUS_LOG(LOG_HUGE, "Sending %s API response to %s (%p)\n", request->admin ? "admin" : "Janus", request->transport->get_package(), request->instance);
	return request->transport->send_message(request->instance, request->request_id, request->admin, payload);
}

static int janus_process_error_string(janus_request *request, uint64_t session_id, const char *transaction, gint error, gchar *error_string)
{
	if(!request)
		return -1;
	/* Done preparing error */
	JANUS_LOG(LOG_VERB, "[%s] Returning %s API error %d (%s)\n", transaction, request->admin ? "admin" : "Janus", error, error_string);
	/* Prepare JSON error */
	json_t *reply = janus_create_message("error", session_id, transaction);
	json_t *error_data = json_object();
	json_object_set_new(error_data, "code", json_integer(error));
	json_object_set_new(error_data, "reason", json_string(error_string));
	json_object_set_new(reply, "error", error_data);
	/* Pass to the right transport plugin */
	return request->transport->send_message(request->instance, request->request_id, request->admin, reply);
}

int janus_process_error(janus_request *request, uint64_t session_id, const char *transaction, gint error, const char *format, ...)
{
	if(!request)
		return -1;
	gchar *error_string = NULL;
	gchar error_buf[512];
	if(format == NULL) {
		/* No error string provided, use the default one */
		error_string = (gchar *)janus_get_api_error(error);
	} else {
		/* This callback has variable arguments (error string) */
		va_list ap;
		va_start(ap, format);
		g_vsnprintf(error_buf, sizeof(error_buf), format, ap);
		va_end(ap);
		error_string = error_buf;
	}
	return janus_process_error_string(request, session_id, transaction, error, error_string);
}

/* Admin/monitor helpers */
json_t *janus_admin_stream_summary(janus_ice_stream *stream) {
	if(stream == NULL)
		return NULL;
	json_t *s = json_object();
	json_object_set_new(s, "id", json_integer(stream->stream_id));
	json_object_set_new(s, "ready", json_integer(stream->cdone));
	json_t *ss = json_object();
	if(stream->audio_ssrc)
		json_object_set_new(ss, "audio", json_integer(stream->audio_ssrc));
	if(stream->video_ssrc)
		json_object_set_new(ss, "video", json_integer(stream->video_ssrc));
	if(stream->video_ssrc_rtx)
		json_object_set_new(ss, "video-rtx", json_integer(stream->video_ssrc_rtx));
	if(stream->audio_ssrc_peer)
		json_object_set_new(ss, "audio-peer", json_integer(stream->audio_ssrc_peer));
	if(stream->video_ssrc_peer[0])
		json_object_set_new(ss, "video-peer", json_integer(stream->video_ssrc_peer[0]));
	if(stream->video_ssrc_peer[1])
		json_object_set_new(ss, "video-peer-sim-1", json_integer(stream->video_ssrc_peer[1]));
	if(stream->video_ssrc_peer[2])
		json_object_set_new(ss, "video-peer-sim-2", json_integer(stream->video_ssrc_peer[2]));
	if(stream->video_ssrc_peer_rtx[0])
		json_object_set_new(ss, "video-peer-rtx", json_integer(stream->video_ssrc_peer_rtx[0]));
	if(stream->video_ssrc_peer_rtx[1])
		json_object_set_new(ss, "video-peer-sim-1-rtx", json_integer(stream->video_ssrc_peer_rtx[1]));
	if(stream->video_ssrc_peer_rtx[2])
		json_object_set_new(ss, "video-peer-sim-2-rtx", json_integer(stream->video_ssrc_peer_rtx[2]));
	if(stream->rid[0]) {
		json_t *rid = json_array();
		json_array_append_new(rid, json_string(stream->rid[0]));
		if(stream->rid[1])
			json_array_append_new(rid, json_string(stream->rid[1]));
		if(stream->rid[1])
			json_array_append_new(rid, json_string(stream->rid[2]));
		json_object_set_new(ss, "rid", rid);
	}
	json_object_set_new(s, "ssrc", ss);
	json_t *sd = json_object();
	json_object_set_new(sd, "audio-send", stream->audio_send ? json_true() : json_false());
	json_object_set_new(sd, "audio-recv", stream->audio_recv ? json_true() : json_false());
	json_object_set_new(sd, "video-send", stream->video_send ? json_true() : json_false());
	json_object_set_new(sd, "video-recv", stream->video_recv ? json_true() : json_false());
	json_object_set_new(s, "direction", sd);
	if(stream->audio_payload_type > -1 || stream->video_payload_type > -1) {
		json_t *sc = json_object();
		if(stream->audio_payload_type > -1)
			json_object_set_new(sc, "audio-pt", json_integer(stream->audio_payload_type));
		if(stream->audio_codec != NULL)
			json_object_set_new(sc, "audio-codec", json_string(stream->audio_codec));
		if(stream->video_payload_type > -1)
			json_object_set_new(sc, "video-pt", json_integer(stream->video_payload_type));
		if(stream->video_rtx_payload_type > -1)
			json_object_set_new(sc, "video-rtx-pt", json_integer(stream->video_rtx_payload_type));
		if(stream->video_codec != NULL)
			json_object_set_new(sc, "video-codec", json_string(stream->video_codec));
		json_object_set_new(s, "codecs", sc);
	}
	json_t *components = json_array();
	if(stream->component) {
		json_t *c = janus_admin_component_summary(stream->component);
		if(c)
			json_array_append_new(components, c);
	}
	json_t *rtcp_stats = NULL;
	if(stream->audio_rtcp_ctx != NULL) {
		rtcp_stats = json_object();
		json_t *audio_rtcp_stats = json_object();
		json_object_set_new(audio_rtcp_stats, "base", json_integer(stream->audio_rtcp_ctx->tb));
		json_object_set_new(audio_rtcp_stats, "rtt", json_integer(janus_rtcp_context_get_rtt(stream->audio_rtcp_ctx)));
		json_object_set_new(audio_rtcp_stats, "lost", json_integer(janus_rtcp_context_get_lost_all(stream->audio_rtcp_ctx, FALSE)));
		json_object_set_new(audio_rtcp_stats, "lost-by-remote", json_integer(janus_rtcp_context_get_lost_all(stream->audio_rtcp_ctx, TRUE)));
		json_object_set_new(audio_rtcp_stats, "jitter-local", json_integer(janus_rtcp_context_get_jitter(stream->audio_rtcp_ctx, FALSE)));
		json_object_set_new(audio_rtcp_stats, "jitter-remote", json_integer(janus_rtcp_context_get_jitter(stream->audio_rtcp_ctx, TRUE)));
		json_object_set_new(rtcp_stats, "audio", audio_rtcp_stats);
	}
	int vindex=0;
	for(vindex=0; vindex<3; vindex++) {
		if(stream->video_rtcp_ctx[vindex] != NULL) {
			if(rtcp_stats == NULL)
				rtcp_stats = json_object();
			json_t *video_rtcp_stats = json_object();
			json_object_set_new(video_rtcp_stats, "base", json_integer(stream->video_rtcp_ctx[vindex]->tb));
			if(vindex == 0)
				json_object_set_new(video_rtcp_stats, "rtt", json_integer(janus_rtcp_context_get_rtt(stream->video_rtcp_ctx[vindex])));
			json_object_set_new(video_rtcp_stats, "lost", json_integer(janus_rtcp_context_get_lost_all(stream->video_rtcp_ctx[vindex], FALSE)));
			json_object_set_new(video_rtcp_stats, "lost-by-remote", json_integer(janus_rtcp_context_get_lost_all(stream->video_rtcp_ctx[vindex], TRUE)));
			json_object_set_new(video_rtcp_stats, "jitter-local", json_integer(janus_rtcp_context_get_jitter(stream->video_rtcp_ctx[vindex], FALSE)));
			json_object_set_new(video_rtcp_stats, "jitter-remote", json_integer(janus_rtcp_context_get_jitter(stream->video_rtcp_ctx[vindex], TRUE)));
			if(vindex == 0)
				json_object_set_new(rtcp_stats, "video", video_rtcp_stats);
			else if(vindex == 1)
				json_object_set_new(rtcp_stats, "video-sim1", video_rtcp_stats);
			else
				json_object_set_new(rtcp_stats, "video-sim2", video_rtcp_stats);
		}
	}
	if(rtcp_stats != NULL)
		json_object_set_new(s, "rtcp_stats", rtcp_stats);
	json_object_set_new(s, "components", components);
	return s;
}

json_t *janus_admin_component_summary(janus_ice_component *component) {
	if(component == NULL)
		return NULL;
	janus_ice_handle *handle = component->stream ? component->stream->handle : NULL;
	json_t *c = json_object();
	json_object_set_new(c, "id", json_integer(component->component_id));
	json_object_set_new(c, "state", json_string(janus_get_ice_state_name(component->state)));
	if(component->icefailed_detected) {
		json_object_set_new(c, "failed-detected", json_integer(component->icefailed_detected));
		json_object_set_new(c, "icetimer-started", component->icestate_source ? json_true() : json_false());
	}
	if(component->component_connected > 0)
		json_object_set_new(c, "connected", json_integer(component->component_connected));
	if(component->local_candidates) {
		json_t *cs = json_array();
		GSList *candidates = component->local_candidates, *i = NULL;
		for (i = candidates; i; i = i->next) {
			gchar *lc = (gchar *) i->data;
			if(lc)
				json_array_append_new(cs, json_string(lc));
		}
		json_object_set_new(c, "local-candidates", cs);
	}
	if(component->remote_candidates) {
		json_t *cs = json_array();
		GSList *candidates = component->remote_candidates, *i = NULL;
		for (i = candidates; i; i = i->next) {
			gchar *rc = (gchar *) i->data;
			if(rc)
				json_array_append_new(cs, json_string(rc));
		}
		json_object_set_new(c, "remote-candidates", cs);
	}
	if(component->selected_pair) {
		json_object_set_new(c, "selected-pair", json_string(component->selected_pair));
	}
	json_t *d = json_object();
	json_t *in_stats = json_object();
	json_t *out_stats = json_object();
	if(component->dtls) {
		janus_dtls_srtp *dtls = component->dtls;
		json_object_set_new(d, "fingerprint", json_string(janus_dtls_get_local_fingerprint()));
		if(component->stream) {
			if(component->stream->remote_fingerprint)
				json_object_set_new(d, "remote-fingerprint", json_string(component->stream->remote_fingerprint));
			if(component->stream->remote_hashing)
				json_object_set_new(d, "remote-fingerprint-hash", json_string(component->stream->remote_hashing));
			json_object_set_new(d, "dtls-role", json_string(janus_get_dtls_srtp_role(component->stream->dtls_role)));
		}
		json_object_set_new(d, "dtls-state", json_string(janus_get_dtls_srtp_state(dtls->dtls_state)));
		json_object_set_new(d, "retransmissions", json_integer(dtls->retransmissions));
		json_object_set_new(d, "valid", dtls->srtp_valid ? json_true() : json_false());
		json_object_set_new(d, "ready", dtls->ready ? json_true() : json_false());
		if(dtls->dtls_started > 0)
			json_object_set_new(d, "handshake-started", json_integer(dtls->dtls_started));
		if(dtls->dtls_connected > 0)
			json_object_set_new(d, "connected", json_integer(dtls->dtls_connected));
		if(handle && janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_AUDIO)) {
			json_object_set_new(in_stats, "audio_packets", json_integer(component->in_stats.audio.packets));
			json_object_set_new(in_stats, "audio_bytes", json_integer(component->in_stats.audio.bytes));
			json_object_set_new(in_stats, "audio_bytes_lastsec", json_integer(component->in_stats.audio.bytes_lastsec));
			json_object_set_new(in_stats, "do_audio_nacks", component->do_audio_nacks ? json_true() : json_false());
			if(component->do_audio_nacks)
				json_object_set_new(in_stats, "audio_nacks", json_integer(component->in_stats.audio.nacks));
		}
		if(handle && janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_VIDEO)) {
			int vindex=0;
			for(vindex=0; vindex<3; vindex++) {
				if(vindex > 0 && component->stream->video_ssrc_peer[vindex] == 0)
					continue;
				json_t *container = (vindex == 0 ? in_stats : json_object());
				json_object_set_new(container, "video_packets", json_integer(component->in_stats.video[vindex].packets));
				json_object_set_new(container, "video_bytes", json_integer(component->in_stats.video[vindex].bytes));
				json_object_set_new(container, "video_bytes_lastsec", json_integer(component->in_stats.video[vindex].bytes_lastsec));
				if(vindex == 0)
					json_object_set_new(container, "do_video_nacks", component->do_video_nacks ? json_true() : json_false());
				if(component->do_video_nacks)
					json_object_set_new(container, "video_nacks", json_integer(component->in_stats.video[vindex].nacks));
				if(vindex == 1)
					json_object_set_new(in_stats, "video-simulcast-1", container);
				else if(vindex == 2)
					json_object_set_new(in_stats, "video-simulcast-2", container);
			}
		}
		json_object_set_new(in_stats, "data_packets", json_integer(component->in_stats.data.packets));
		json_object_set_new(in_stats, "data_bytes", json_integer(component->in_stats.data.bytes));
		if(handle && janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_AUDIO)) {
			json_object_set_new(out_stats, "audio_packets", json_integer(component->out_stats.audio.packets));
			json_object_set_new(out_stats, "audio_bytes", json_integer(component->out_stats.audio.bytes));
			json_object_set_new(out_stats, "audio_bytes_lastsec", json_integer(component->out_stats.audio.bytes_lastsec));
			json_object_set_new(out_stats, "audio_nacks", json_integer(component->out_stats.audio.nacks));
		}
		if(handle && janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_HAS_VIDEO)) {
			json_object_set_new(out_stats, "video_packets", json_integer(component->out_stats.video[0].packets));
			json_object_set_new(out_stats, "video_bytes", json_integer(component->out_stats.video[0].bytes));
			json_object_set_new(out_stats, "video_bytes_lastsec", json_integer(component->out_stats.video[0].bytes_lastsec));
			json_object_set_new(out_stats, "video_nacks", json_integer(component->out_stats.video[0].nacks));
		}
		json_object_set_new(out_stats, "data_packets", json_integer(component->out_stats.data.packets));
		json_object_set_new(out_stats, "data_bytes", json_integer(component->out_stats.data.bytes));
#ifdef HAVE_SCTP
		/* FIXME Actually check if this succeeded? */
		json_object_set_new(d, "sctp-association", dtls->sctp ? json_true() : json_false());
#endif
	}
	json_object_set_new(c, "dtls", d);
	json_object_set_new(c, "in_stats", in_stats);
	json_object_set_new(c, "out_stats", out_stats);
	return c;
}


/* Transports */
void janus_transport_close(gpointer key, gpointer value, gpointer user_data) {
	janus_transport *transport = (janus_transport *)value;
	if(!transport)
		return;
	transport->destroy();
}

void janus_transportso_close(gpointer key, gpointer value, gpointer user_data) {
	void *transport = value;
	if(!transport)
		return;
	/* FIXME We don't dlclose transports to be sure we can detect leaks */
	//~ dlclose(transport);
}

/* Transport callback interface */
void janus_transport_incoming_request(janus_transport *plugin, janus_transport_session *transport, void *request_id, gboolean admin, json_t *message, json_error_t *error) {
	JANUS_LOG(LOG_VERB, "Got %s API request from %s (%p)\n", admin ? "an admin" : "a Janus", plugin->get_package(), transport);
	/* Create a janus_request instance to handle the request */
	janus_request *request = janus_request_new(plugin, transport, request_id, admin, message);
	/* Enqueue the request, the thread will pick it up */
	g_async_queue_push(requests, request);
}

void janus_transport_gone(janus_transport *plugin, janus_transport_session *transport) {
	/* Get rid of sessions this transport was handling */
	JANUS_LOG(LOG_VERB, "A %s transport instance has gone away (%p)\n", plugin->get_package(), transport);
	janus_mutex_lock(&sessions_mutex);
	if(sessions && g_hash_table_size(sessions) > 0) {
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, sessions);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			janus_session *session = (janus_session *) value;
			if(!session || g_atomic_int_get(&session->destroyed) || g_atomic_int_get(&session->timeout) || session->last_activity == 0)
				continue;
			if(session->source && session->source->instance == transport) {
				JANUS_LOG(LOG_VERB, "  -- Marking Session %"SCNu64" as over\n", session->session_id);
				/* Mark the session as destroyed */
				janus_session_destroy(session);
				g_hash_table_iter_remove(&iter);
			}
		}
	}
	janus_mutex_unlock(&sessions_mutex);
}

gboolean janus_transport_is_api_secret_needed(janus_transport *plugin) {
	return api_secret != NULL;
}

gboolean janus_transport_is_api_secret_valid(janus_transport *plugin, const char *apisecret) {
	if(api_secret == NULL)
		return TRUE;
	return apisecret && janus_strcmp_const_time(apisecret, api_secret);
}

gboolean janus_transport_is_auth_token_needed(janus_transport *plugin) {
	return janus_auth_is_enabled();
}

gboolean janus_transport_is_auth_token_valid(janus_transport *plugin, const char *token) {
	if(!janus_auth_is_enabled())
		return TRUE;
	return token && janus_auth_check_token(token);
}

void janus_transport_notify_event(janus_transport *plugin, void *transport, json_t *event) {
	/* A plugin asked to notify an event to the handlers */
	if(!plugin || !event || !json_is_object(event))
		return;
	/* Notify event handlers */
	if(janus_events_is_enabled()) {
		janus_events_notify_handlers(JANUS_EVENT_TYPE_TRANSPORT,
			0, plugin->get_package(), transport, event);
	} else {
		json_decref(event);
	}
}

void janus_transport_task(gpointer data, gpointer user_data) {
	JANUS_LOG(LOG_VERB, "Transport task pool, serving request\n");
	janus_request *request = (janus_request *)data;
	if(request == NULL) {
		JANUS_LOG(LOG_ERR, "Missing request\n");
		return;
	}
	if(!request->admin)
		janus_process_incoming_request(request);
	else
		janus_process_incoming_admin_request(request);
	/* Done */
	janus_request_destroy(request);
}


/* Thread to handle incoming requests: may involve an asynchronous task for plugin messaging */
static void *janus_transport_requests(void *data) {
	JANUS_LOG(LOG_INFO, "Joining Janus requests handler thread\n");
	janus_request *request = NULL;
	gboolean destroy = FALSE;
	while(!g_atomic_int_get(&stop)) {
		request = g_async_queue_pop(requests);
		if(request == &exit_message)
			break;
		/* Should we process the request synchronously or with a task from the thread pool? */
		destroy = TRUE;
		if(!request->admin) {
			/* Process the request synchronously only it's not a message for a plugin */
			json_t *message = json_object_get(request->message, "janus");
			const gchar *message_text = json_string_value(message);
			if(message_text && !strcasecmp(message_text, "message")) {
				/* Spawn a task thread */
				GError *tperror = NULL;
				g_thread_pool_push(tasks, request, &tperror);
				if(tperror != NULL) {
					/* Something went wrong... */
					JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to push task in thread pool...\n", tperror->code, tperror->message ? tperror->message : "??");
					json_t *transaction = json_object_get(message, "transaction");
					const char *transaction_text = json_is_string(transaction) ? json_string_value(transaction) : NULL;
					janus_process_error(request, 0, transaction_text, JANUS_ERROR_UNKNOWN, "Thread pool error");
				} else {
					/* Don't destroy the request now, the task will take care of that */
					destroy = FALSE;
				}
			} else {
				janus_process_incoming_request(request);
			}
		} else {
			/* Admin requests are always handled synchronously */
			janus_process_incoming_admin_request(request);
		}
		/* Done */
		if(destroy)
			janus_request_destroy(request);
	}
	JANUS_LOG(LOG_INFO, "Leaving Janus requests handler thread\n");
	return NULL;
}


/* Event handlers */
void janus_eventhandler_close(gpointer key, gpointer value, gpointer user_data) {
	janus_eventhandler *eventhandler = (janus_eventhandler *)value;
	if(!eventhandler)
		return;
	eventhandler->destroy();
}

void janus_eventhandlerso_close(gpointer key, gpointer value, gpointer user_data) {
	void *eventhandler = (janus_eventhandler *)value;
	if(!eventhandler)
		return;
	//~ dlclose(eventhandler);
}


/* Plugins */
void janus_plugin_close(gpointer key, gpointer value, gpointer user_data) {
	janus_plugin *plugin = (janus_plugin *)value;
	if(!plugin)
		return;
	plugin->destroy();
}

void janus_pluginso_close(gpointer key, gpointer value, gpointer user_data) {
	void *plugin = value;
	if(!plugin)
		return;
	/* FIXME We don't dlclose plugins to be sure we can detect leaks */
	//~ dlclose(plugin);
}

janus_plugin *janus_plugin_find(const gchar *package) {
	if(package != NULL && plugins != NULL)	/* FIXME Do we need to fix the key pointer? */
		return g_hash_table_lookup(plugins, package);
	return NULL;
}


/* Plugin callback interface */
int janus_plugin_push_event(janus_plugin_session *plugin_session, janus_plugin *plugin, const char *transaction, json_t *message, json_t *jsep) {
	if(!plugin || !message)
		return -1;
	if(!plugin_session || plugin_session < (janus_plugin_session *)0x1000 ||
			!janus_plugin_session_is_alive(plugin_session) || g_atomic_int_get(&plugin_session->stopped))
		return -2;
	janus_refcount_increase(&plugin_session->ref);
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!ice_handle || janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)) {
		janus_refcount_decrease(&plugin_session->ref);
		return JANUS_ERROR_SESSION_NOT_FOUND;
	}
	janus_refcount_increase(&ice_handle->ref);
	janus_session *session = ice_handle->session;
	if(!session || g_atomic_int_get(&session->destroyed)) {
		janus_refcount_decrease(&plugin_session->ref);
		janus_refcount_decrease(&ice_handle->ref);
		return JANUS_ERROR_SESSION_NOT_FOUND;
	}
	/* Make sure this is a JSON object */
	if(!json_is_object(message)) {
		JANUS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (JSON error: not an object)\n", ice_handle->handle_id);
		janus_refcount_decrease(&plugin_session->ref);
		janus_refcount_decrease(&ice_handle->ref);
		return JANUS_ERROR_INVALID_JSON_OBJECT;
	}
	/* Attach JSEP if possible? */
	const char *sdp_type = json_string_value(json_object_get(jsep, "type"));
	const char *sdp = json_string_value(json_object_get(jsep, "sdp"));
	gboolean restart = json_object_get(jsep, "sdp") ? json_is_true(json_object_get(jsep, "restart")) : FALSE;
	json_t *merged_jsep = NULL;
	if(sdp_type != NULL && sdp != NULL) {
		merged_jsep = janus_plugin_handle_sdp(plugin_session, plugin, sdp_type, sdp, restart);
		if(merged_jsep == NULL) {
			if(ice_handle == NULL || janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
					|| janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				JANUS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (handle not available anymore or negotiation stopped)\n", ice_handle->handle_id);
				janus_refcount_decrease(&plugin_session->ref);
				janus_refcount_decrease(&ice_handle->ref);
				return JANUS_ERROR_HANDLE_NOT_FOUND;
			} else {
				JANUS_LOG(LOG_ERR, "[%"SCNu64"] Cannot push event (JSON error: problem with the SDP)\n", ice_handle->handle_id);
				janus_refcount_decrease(&plugin_session->ref);
				janus_refcount_decrease(&ice_handle->ref);
				return JANUS_ERROR_JSEP_INVALID_SDP;
			}
		}
	}
	/* Reference the payload, as the plugin may still need it and will do a decref itself */
	json_incref(message);
	/* Prepare JSON event */
	json_t *event = janus_create_message("event", session->session_id, transaction);
	json_object_set_new(event, "sender", json_integer(ice_handle->handle_id));
	json_t *plugin_data = json_object();
	json_object_set_new(plugin_data, "plugin", json_string(plugin->get_package()));
	json_object_set_new(plugin_data, "data", message);
	json_object_set_new(event, "plugindata", plugin_data);
	if(merged_jsep != NULL)
		json_object_set_new(event, "jsep", merged_jsep);
	/* Send the event */
	JANUS_LOG(LOG_VERB, "[%"SCNu64"] Sending event to transport...\n", ice_handle->handle_id);
	janus_session_notify_event(session, event);

	if((restart || janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RESEND_TRICKLES))
			&& janus_ice_is_full_trickle_enabled()) {
		/* We're restarting ICE, send our trickle candidates again */
		janus_ice_resend_trickles(ice_handle);
	}

	if(jsep != NULL && janus_events_is_enabled()) {
		/* Notify event handlers as well */
		janus_events_notify_handlers(JANUS_EVENT_TYPE_JSEP,
			session->session_id, ice_handle->handle_id, ice_handle->opaque_id, "local", sdp_type, sdp);
	}

	janus_refcount_decrease(&plugin_session->ref);
	janus_refcount_decrease(&ice_handle->ref);
	return JANUS_OK;
}

json_t *janus_plugin_handle_sdp(janus_plugin_session *plugin_session, janus_plugin *plugin, const char *sdp_type, const char *sdp, gboolean restart) {
	if(!plugin_session || plugin_session < (janus_plugin_session *)0x1000 ||
			!janus_plugin_session_is_alive(plugin_session) || g_atomic_int_get(&plugin_session->stopped) ||
			plugin == NULL || sdp_type == NULL || sdp == NULL) {
		JANUS_LOG(LOG_ERR, "Invalid arguments\n");
		return NULL;
	}
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	//~ if(ice_handle == NULL || janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_READY)) {
	if(ice_handle == NULL) {
		JANUS_LOG(LOG_ERR, "Invalid ICE handle\n");
		return NULL;
	}
	int offer = 0;
	if(!strcasecmp(sdp_type, "offer")) {
		/* This is an offer from a plugin */
		offer = 1;
		janus_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_OFFER);
		janus_flags_clear(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
	} else if(!strcasecmp(sdp_type, "answer")) {
		/* This is an answer from a plugin */
		janus_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_GOT_ANSWER);
	} else {
		/* TODO Handle other messages */
		JANUS_LOG(LOG_ERR, "Unknown type '%s'\n", sdp_type);
		return NULL;
	}
	/* Is this valid SDP? */
	char error_str[512];
	int audio = 0, video = 0, data = 0;
	janus_sdp *parsed_sdp = janus_sdp_preparse(sdp, error_str, sizeof(error_str), &audio, &video, &data);
	if(parsed_sdp == NULL) {
		JANUS_LOG(LOG_ERR, "[%"SCNu64"] Couldn't parse SDP... %s\n", ice_handle->handle_id, error_str);
		return NULL;
	}
	gboolean updating = FALSE;
	if(offer) {
		/* We may still not have a local ICE setup */
		JANUS_LOG(LOG_VERB, "[%"SCNu64"] Audio %s been negotiated\n", ice_handle->handle_id, audio ? "has" : "has NOT");
		if(audio > 1) {
			JANUS_LOG(LOG_ERR, "[%"SCNu64"] More than one audio line? only going to negotiate one...\n", ice_handle->handle_id);
		}
		JANUS_LOG(LOG_VERB, "[%"SCNu64"] Video %s been negotiated\n", ice_handle->handle_id, video ? "has" : "has NOT");
		if(video > 1) {
			JANUS_LOG(LOG_ERR, "[%"SCNu64"] More than one video line? only going to negotiate one...\n", ice_handle->handle_id);
		}
		JANUS_LOG(LOG_VERB, "[%"SCNu64"] SCTP/DataChannels %s been negotiated\n", ice_handle->handle_id, data ? "have" : "have NOT");
		if(data > 1) {
			JANUS_LOG(LOG_ERR, "[%"SCNu64"] More than one data line? only going to negotiate one...\n", ice_handle->handle_id);
		}
#ifndef HAVE_SCTP
		if(data) {
			JANUS_LOG(LOG_WARN, "[%"SCNu64"]   -- DataChannels have been negotiated, but support for them has not been compiled...\n", ice_handle->handle_id);
		}
#endif
		/* Are we still cleaning up from a previous media session? */
		if(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", ice_handle->handle_id);
			gint64 waited = 0;
			while(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_CLEANING)) {
				JANUS_LOG(LOG_VERB, "[%"SCNu64"] Still cleaning up from a previous media session, let's wait a bit...\n", ice_handle->handle_id);
				g_usleep(100000);
				waited += 100000;
				if(waited >= 3*G_USEC_PER_SEC) {
					JANUS_LOG(LOG_VERB, "[%"SCNu64"]   -- Waited 3 seconds, that's enough!\n", ice_handle->handle_id);
					JANUS_LOG(LOG_ERR, "[%"SCNu64"] Still cleaning a previous session\n", ice_handle->handle_id);
					janus_sdp_destroy(parsed_sdp);
					return NULL;
				}
			}
		}
		if(ice_handle->agent == NULL) {
			if(janus_is_rfc4588_enabled()) {
				/* We still need to configure the WebRTC stuff: negotiate RFC4588 by default */
				janus_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RFC4588_RTX);
			}
			/* Process SDP in order to setup ICE locally (this is going to result in an answer from the browser) */
			if(janus_ice_setup_local(ice_handle, 0, audio, video, data, 1) < 0) {
				JANUS_LOG(LOG_ERR, "[%"SCNu64"] Error setting ICE locally\n", ice_handle->handle_id);
				janus_sdp_destroy(parsed_sdp);
				return NULL;
			}
		} else {
			updating = TRUE;
			JANUS_LOG(LOG_INFO, "[%"SCNu64"] Updating existing session\n", ice_handle->handle_id);
		}
	} else {
		/* Check if transport wide CC is supported */
		int transport_wide_cc_ext_id = janus_rtp_header_extension_get_id(sdp, JANUS_RTP_EXTMAP_TRANSPORT_WIDE_CC);
		ice_handle->stream->do_transport_wide_cc = TRUE;
		ice_handle->stream->transport_wide_cc_ext_id = transport_wide_cc_ext_id;
	}
	if(!updating) {
		/* Wait for candidates-done callback */
		while(ice_handle->cdone < 1) {
			if(ice_handle == NULL || janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
					|| janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
				JANUS_LOG(LOG_WARN, "[%"SCNu64"] Handle detached or PC closed, giving up...!\n", ice_handle ? ice_handle->handle_id : 0);
				janus_sdp_destroy(parsed_sdp);
				return NULL;
			}
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Waiting for candidates-done callback...\n", ice_handle->handle_id);
			g_usleep(100000);
			if(ice_handle->cdone < 0) {
				JANUS_LOG(LOG_ERR, "[%"SCNu64"] Error gathering candidates!\n", ice_handle->handle_id);
				janus_sdp_destroy(parsed_sdp);
				return NULL;
			}
		}
	}
	/* Anonymize SDP */
	if(janus_sdp_anonymize(parsed_sdp) < 0) {
		/* Invalid SDP */
		JANUS_LOG(LOG_ERR, "[%"SCNu64"] Invalid SDP\n", ice_handle->handle_id);
		janus_sdp_destroy(parsed_sdp);
		return NULL;
	}
	/* Check if this is a renegotiation and we need an ICE restart */
	if(offer && restart)
		janus_ice_restart(ice_handle);
	/* Add our details */
	janus_ice_stream *stream = ice_handle->stream;
	if(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_RFC4588_RTX) &&
			stream && stream->rtx_payload_types == NULL) {
		/* Make sure we have a list of rtx payload types to generate, if needed */
		janus_sdp_mline *m = janus_sdp_mline_find(parsed_sdp, JANUS_SDP_VIDEO);
		if(m && m->ptypes) {
			stream->rtx_payload_types = g_hash_table_new(NULL, NULL);
			GList *ptypes = g_list_copy(m->ptypes), *tempP = ptypes;
			GList *rtx_ptypes = g_hash_table_get_values(stream->rtx_payload_types);
			while(tempP) {
				int ptype = GPOINTER_TO_INT(tempP->data);
				int rtx_ptype = ptype+1;
				while(g_list_find(m->ptypes, GINT_TO_POINTER(rtx_ptype))
						|| g_list_find(rtx_ptypes, GINT_TO_POINTER(rtx_ptype))) {
					rtx_ptype++;
					if(rtx_ptype > 127)
						rtx_ptype = 96;
					if(rtx_ptype == ptype) {
						/* We did a whole round? should never happen... */
						rtx_ptype = -1;
						break;
					}
				}
				if(rtx_ptype > 0)
					g_hash_table_insert(stream->rtx_payload_types, GINT_TO_POINTER(ptype), GINT_TO_POINTER(rtx_ptype));
				g_list_free(rtx_ptypes);
				rtx_ptypes = g_hash_table_get_values(stream->rtx_payload_types);
				tempP = tempP->next;
			}
			g_list_free(ptypes);
			g_list_free(rtx_ptypes);
		}
	}
	/* Enrich the SDP the plugin gave us with all the WebRTC related stuff */
	char *sdp_merged = janus_sdp_merge(ice_handle, parsed_sdp, offer ? TRUE : FALSE);
	if(sdp_merged == NULL) {
		/* Couldn't merge SDP */
		JANUS_LOG(LOG_ERR, "[%"SCNu64"] Error merging SDP\n", ice_handle->handle_id);
		janus_sdp_destroy(parsed_sdp);
		return NULL;
	}
	janus_sdp_destroy(parsed_sdp);

	if(!updating) {
		if(offer) {
			/* We set the flag to wait for an answer before handling trickle candidates */
			janus_flags_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_PROCESSING_OFFER);
		} else {
			JANUS_LOG(LOG_VERB, "[%"SCNu64"] Done! Ready to setup remote candidates and send connectivity checks...\n", ice_handle->handle_id);
			janus_mutex_lock(&ice_handle->mutex);
			janus_request_ice_handle_answer(ice_handle, audio, video, data, NULL);
			janus_mutex_unlock(&ice_handle->mutex);
		}
	}
#ifdef HAVE_SCTP
	if(!offer && janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_READY)) {
		/* Renegotiation: check if datachannels were just added on an existing PeerConnection */
		if(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_DATA_CHANNELS)) {
			janus_ice_stream *stream = ice_handle->stream;
			if(stream != NULL && stream->component != NULL &&
					stream->component->dtls != NULL && stream->component->dtls->sctp == NULL) {
				/* Create SCTP association as well */
				JANUS_LOG(LOG_WARN, "[%"SCNu64"] Creating datachannels...\n", ice_handle->handle_id);
				janus_dtls_srtp_create_sctp(stream->component->dtls);
			}
		}
	}
#endif

	/* Prepare JSON event */
	json_t *jsep = json_object();
	json_object_set_new(jsep, "type", json_string(sdp_type));
	json_object_set_new(jsep, "sdp", json_string(sdp_merged));
	char *tmp = ice_handle->local_sdp;
	ice_handle->local_sdp = sdp_merged;
	g_free(tmp);
	return jsep;
}

void janus_plugin_relay_rtp(janus_plugin_session *plugin_session, int video, char *buf, int len) {
	if((plugin_session < (janus_plugin_session *)0x1000) || g_atomic_int_get(&plugin_session->stopped) || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
	janus_ice_relay_rtp(handle, video, buf, len);
}

void janus_plugin_relay_rtcp(janus_plugin_session *plugin_session, int video, char *buf, int len) {
	if((plugin_session < (janus_plugin_session *)0x1000) || g_atomic_int_get(&plugin_session->stopped) || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
	janus_ice_relay_rtcp(handle, video, buf, len);
}

void janus_plugin_relay_data(janus_plugin_session *plugin_session, char *buf, int len) {
	if((plugin_session < (janus_plugin_session *)0x1000) || g_atomic_int_get(&plugin_session->stopped) || buf == NULL || len < 1)
		return;
	janus_ice_handle *handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!handle || janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| janus_flags_is_set(&handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT))
		return;
#ifdef HAVE_SCTP
	janus_ice_relay_data(handle, buf, len);
#else
	JANUS_LOG(LOG_WARN, "Asked to relay data, but Data Channels support has not been compiled...\n");
#endif
}

static gboolean janus_plugin_close_pc_internal(gpointer user_data) {
	/* We actually enforce the close_pc here */
	janus_plugin_session *plugin_session = (janus_plugin_session *) user_data;
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!ice_handle) {
		janus_refcount_decrease(&plugin_session->ref);
		return G_SOURCE_REMOVE;
	}
	janus_refcount_increase(&ice_handle->ref);
	if(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
		janus_refcount_decrease(&plugin_session->ref);
		janus_refcount_decrease(&ice_handle->ref);
		return G_SOURCE_REMOVE;
	}
	JANUS_LOG(LOG_VERB, "[%"SCNu64"] Plugin asked to hangup PeerConnection: sending alert\n", ice_handle->handle_id);
	/* Send an alert on all the DTLS connections */
	janus_ice_webrtc_hangup(ice_handle, "Close PC");
	janus_refcount_decrease(&plugin_session->ref);
	janus_refcount_decrease(&ice_handle->ref);

	return G_SOURCE_REMOVE;
}

void janus_plugin_close_pc(janus_plugin_session *plugin_session) {
	/* A plugin asked to get rid of a PeerConnection: enqueue it as a timed source */
	if((plugin_session < (janus_plugin_session *)0x1000) || !janus_plugin_session_is_alive(plugin_session) || g_atomic_int_get(&plugin_session->stopped))
		return;
	janus_refcount_increase(&plugin_session->ref);
	GSource *timeout_source = g_timeout_source_new_seconds(0);
	g_source_set_callback(timeout_source, janus_plugin_close_pc_internal, plugin_session, NULL);
	g_source_attach(timeout_source, sessions_watchdog_context);
	g_source_unref(timeout_source);
}

static gboolean janus_plugin_end_session_internal(gpointer user_data) {
	/* We actually enforce the end_session here */
	janus_plugin_session *plugin_session = (janus_plugin_session *) user_data;
	janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
	if(!ice_handle) {
		janus_refcount_decrease(&plugin_session->ref);
		return G_SOURCE_REMOVE;
	}
	janus_refcount_increase(&ice_handle->ref);
	if(janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_STOP)
			|| janus_flags_is_set(&ice_handle->webrtc_flags, JANUS_ICE_HANDLE_WEBRTC_ALERT)) {
		janus_refcount_decrease(&plugin_session->ref);
		janus_refcount_decrease(&ice_handle->ref);
		return G_SOURCE_REMOVE;
	}
	janus_session *session = (janus_session *)ice_handle->session;
	if(!session) {
		janus_refcount_decrease(&plugin_session->ref);
		janus_refcount_decrease(&ice_handle->ref);
		return G_SOURCE_REMOVE;
	}
	/* Destroy the handle */
	janus_session_handles_remove(session, ice_handle);

	return G_SOURCE_REMOVE;
}

void janus_plugin_end_session(janus_plugin_session *plugin_session) {
	/* A plugin asked to get rid of a handle: enqueue it as a timed source */
	if((plugin_session < (janus_plugin_session *)0x1000) || !janus_plugin_session_is_alive(plugin_session) || g_atomic_int_get(&plugin_session->stopped))
		return;
	GSource *timeout_source = g_timeout_source_new_seconds(0);
	g_source_set_callback(timeout_source, janus_plugin_end_session_internal, plugin_session, NULL);
	g_source_attach(timeout_source, sessions_watchdog_context);
	g_source_unref(timeout_source);
}

void janus_plugin_notify_event(janus_plugin *plugin, janus_plugin_session *plugin_session, json_t *event) {
	/* A plugin asked to notify an event to the handlers */
	if(!plugin || !event || !json_is_object(event))
		return;
	guint64 session_id = 0, handle_id = 0;
	char *opaque_id = NULL;
	if(plugin_session != NULL) {
		if((plugin_session < (janus_plugin_session *)0x1000) || !janus_plugin_session_is_alive(plugin_session) || plugin_session->stopped) {
			json_decref(event);
			return;
		}
		janus_ice_handle *ice_handle = (janus_ice_handle *)plugin_session->gateway_handle;
		if(!ice_handle) {
			json_decref(event);
			return;
		}
		handle_id = ice_handle->handle_id;
		opaque_id = ice_handle->opaque_id;
		janus_session *session = (janus_session *)ice_handle->session;
		if(!session) {
			json_decref(event);
			return;
		}
		session_id = session->session_id;
	}
	/* Notify event handlers */
	if(janus_events_is_enabled()) {
		janus_events_notify_handlers(JANUS_EVENT_TYPE_PLUGIN,
			session_id, handle_id, opaque_id, plugin->get_package(), event);
	} else {
		json_decref(event);
	}
}


/* Main */
gint main(int argc, char *argv[])
{
	/* Core dumps may be disallowed by parent of this process; change that */
	struct rlimit core_limits;
	core_limits.rlim_cur = core_limits.rlim_max = RLIM_INFINITY;
	setrlimit(RLIMIT_CORE, &core_limits);

	g_print("Janus commit: %s\n", janus_build_git_sha);
	g_print("Compiled on:  %s\n\n", janus_build_git_time);

	struct gengetopt_args_info args_info;
	/* Let's call our cmdline parser */
	if(cmdline_parser(argc, argv, &args_info) != 0)
		exit(1);

	/* Any configuration to open? */
	if(args_info.config_given) {
		config_file = g_strdup(args_info.config_arg);
	}
	if(args_info.configs_folder_given) {
		configs_folder = g_strdup(args_info.configs_folder_arg);
	} else {
		configs_folder = g_strdup (CONFDIR);
	}
	if(config_file == NULL) {
		char file[255];
		g_snprintf(file, 255, "%s/janus.cfg", configs_folder);
		config_file = g_strdup(file);
	}
	if((config = janus_config_parse(config_file)) == NULL) {
		if(args_info.config_given) {
			/* We only give up if the configuration file was explicitly provided */
			g_print("Error reading configuration from %s\n", configs_folder);
			exit(1);
		}
		g_print("Error reading/parsing the configuration file in %s, going on with the defaults and the command line arguments\n",
			configs_folder);
		config = janus_config_create("janus.cfg");
		if(config == NULL) {
			/* If we can't even create an empty configuration, something's definitely wrong */
			exit(1);
		}
	}

	/* Check if we need to log to console and/or file */
	gboolean use_stdout = TRUE;
	if(args_info.disable_stdout_given) {
		use_stdout = FALSE;
		janus_config_add_item(config, "general", "log_to_stdout", "no");
	} else {
		/* Check if the configuration file is saying anything about this */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "log_to_stdout");
		if(item && item->value && !janus_is_true(item->value))
			use_stdout = FALSE;
	}
	const char *logfile = NULL;
	if(args_info.log_file_given) {
		logfile = args_info.log_file_arg;
		janus_config_add_item(config, "general", "log_to_file", "no");
	} else {
		/* Check if the configuration file is saying anything about this */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "log_to_file");
		if(item && item->value)
			logfile = item->value;
	}

	/* Check if we're going to daemonize Janus */
	if(args_info.daemon_given) {
		daemonize = TRUE;
		janus_config_add_item(config, "general", "daemonize", "yes");
	} else {
		/* Check if the configuration file is saying anything about this */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "daemonize");
		if(item && item->value && janus_is_true(item->value))
			daemonize = TRUE;
	}
	/* If we're going to daemonize, make sure logging to stdout is disabled and a log file has been specified */
	if(daemonize && use_stdout) {
		use_stdout = FALSE;
	}
	if(daemonize && logfile == NULL) {
		g_print("Running Janus as a daemon but no log file provided, giving up...\n");
		exit(1);
	}
	/* Daemonize now, if we need to */
	if(daemonize) {
		g_print("Running Janus as a daemon\n");

		/* Create a pipe for parent<->child communication during the startup phase */
		if(pipe(pipefd) == -1) {
			g_print("pipe error!\n");
			exit(1);
		}

		/* Fork off the parent process */
		pid_t pid = fork();
		if(pid < 0) {
			g_print("Fork error!\n");
			exit(1);
		}
		if(pid > 0) {
			/* Ok, we're the parent: let's wait for the child to tell us everything started fine */
			close(pipefd[1]);
			int code = -1;
			struct pollfd pollfds;

			while(code < 0) {
				pollfds.fd = pipefd[0];
				pollfds.events = POLLIN;
				int res = poll(&pollfds, 1, -1);
				if(res < 0)
					break;
				if(res == 0)
					continue;
				if(pollfds.revents & POLLERR || pollfds.revents & POLLHUP)
					break;
				if(pollfds.revents & POLLIN) {
					res = read(pipefd[0], &code, sizeof(int));
					break;
				}
			}
			if(code < 0)
				code = 1;

			/* Leave the parent and return the exit code we received from the child */
			if(code)
				g_print("Error launching Janus (error code %d), check the logs for more details\n", code);
			exit(code);
		}
		/* Child here */
		close(pipefd[0]);

		/* Change the file mode mask */
		umask(0);

		/* Create a new SID for the child process */
		pid_t sid = setsid();
		if(sid < 0) {
			g_print("Error setting SID!\n");
			exit(1);
		}
		/* Change the current working directory */
		if((chdir("/")) < 0) {
			g_print("Error changing the current working directory!\n");
			exit(1);
		}
		/* We close stdin/stdout/stderr when initializing the logger */
	}

	/* Initialize logger */
	if(janus_log_init(daemonize, use_stdout, logfile) < 0)
		exit(1);

	JANUS_PRINT("---------------------------------------------------\n");
	JANUS_PRINT("  Starting Meetecho Janus (WebRTC Gateway) v%s\n", janus_version_string);
	JANUS_PRINT("---------------------------------------------------\n\n");

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, janus_handle_signal);
	signal(SIGTERM, janus_handle_signal);
	atexit(janus_termination_handler);

	/* Setup Glib */
#if !GLIB_CHECK_VERSION(2, 36, 0)
	g_type_init();
#endif

	/* Logging level: default is info and no timestamps */
	janus_log_level = LOG_INFO;
	janus_log_timestamps = FALSE;
	janus_log_colors = TRUE;
	if(args_info.debug_level_given) {
		if(args_info.debug_level_arg < LOG_NONE)
			args_info.debug_level_arg = 0;
		else if(args_info.debug_level_arg > LOG_MAX)
			args_info.debug_level_arg = LOG_MAX;
		janus_log_level = args_info.debug_level_arg;
	}

	/* Any PID we need to create? */
	const char *pidfile = NULL;
	if(args_info.pid_file_given) {
		pidfile = args_info.pid_file_arg;
		janus_config_add_item(config, "general", "pid_file", pidfile);
	} else {
		/* Check if the configuration file is saying anything about this */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "pid_file");
		if(item && item->value)
			pidfile = item->value;
	}
	if(janus_pidfile_create(pidfile) < 0)
		exit(1);

	/* Proceed with the rest of the configuration */
	janus_config_print(config);
	if(args_info.debug_level_given) {
		char debug[5];
		g_snprintf(debug, 5, "%d", args_info.debug_level_arg);
		janus_config_add_item(config, "general", "debug_level", debug);
	} else {
		/* No command line directive on logging, try the configuration file */
		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "debug_level");
		if(item && item->value) {
			int temp_level = atoi(item->value);
			if(temp_level == 0 && strcmp(item->value, "0")) {
				JANUS_PRINT("Invalid debug level %s (configuration), using default (info=4)\n", item->value);
			} else {
				janus_log_level = temp_level;
				if(janus_log_level < LOG_NONE)
					janus_log_level = 0;
				else if(janus_log_level > LOG_MAX)
					janus_log_level = LOG_MAX;
			}
		}
	}
	/* Any command line argument that should overwrite the configuration? */
	JANUS_PRINT("Checking command line arguments...\n");
	if(args_info.debug_timestamps_given) {
		janus_config_add_item(config, "general", "debug_timestamps", "yes");
	}
	if(args_info.disable_colors_given) {
		janus_config_add_item(config, "general", "debug_colors", "no");
	}
	if(args_info.server_name_given) {
		janus_config_add_item(config, "general", "server_name", args_info.server_name_arg);
	}
	if(args_info.session_timeout_given) {
		char st[20];
		g_snprintf(st, 20, "%d", args_info.session_timeout_arg);
		janus_config_add_item(config, "general", "session_timeout", st);
	}
 	if(args_info.interface_given) {
		janus_config_add_item(config, "general", "interface", args_info.interface_arg);
	}
	if(args_info.configs_folder_given) {
		janus_config_add_item(config, "general", "configs_folder", args_info.configs_folder_arg);
	}
	if(args_info.plugins_folder_given) {
		janus_config_add_item(config, "general", "plugins_folder", args_info.plugins_folder_arg);
	}
	if(args_info.apisecret_given) {
		janus_config_add_item(config, "general", "api_secret", args_info.apisecret_arg);
	}
	if(args_info.token_auth_given) {
		janus_config_add_item(config, "general", "token_auth", "yes");
	}
	if(args_info.cert_pem_given) {
		janus_config_add_item(config, "certificates", "cert_pem", args_info.cert_pem_arg);
	}
	if(args_info.cert_key_given) {
		janus_config_add_item(config, "certificates", "cert_key", args_info.cert_key_arg);
	}
	if(args_info.stun_server_given) {
		/* Split in server and port (if port missing, use 3478 as default) */
		char *stunport = strrchr(args_info.stun_server_arg, ':');
		if(stunport != NULL) {
			*stunport = '\0';
			stunport++;
			janus_config_add_item(config, "nat", "stun_server", args_info.stun_server_arg);
			janus_config_add_item(config, "nat", "stun_port", stunport);
		} else {
			janus_config_add_item(config, "nat", "stun_server", args_info.stun_server_arg);
			janus_config_add_item(config, "nat", "stun_port", "3478");
		}
	}
	if(args_info.nat_1_1_given) {
		janus_config_add_item(config, "nat", "nat_1_1_mapping", args_info.nat_1_1_arg);
	}
	if(args_info.ice_enforce_list_given) {
		janus_config_add_item(config, "nat", "ice_enforce_list", args_info.ice_enforce_list_arg);
	}
	if(args_info.ice_ignore_list_given) {
		janus_config_add_item(config, "nat", "ice_ignore_list", args_info.ice_ignore_list_arg);
	}
	if(args_info.libnice_debug_given) {
		janus_config_add_item(config, "nat", "nice_debug", "true");
	}
	if(args_info.full_trickle_given) {
		janus_config_add_item(config, "nat", "full_trickle", "true");
	}
	if(args_info.ice_lite_given) {
		janus_config_add_item(config, "nat", "ice_lite", "true");
	}
	if(args_info.ice_tcp_given) {
		janus_config_add_item(config, "nat", "ice_tcp", "true");
	}
	if(args_info.ipv6_candidates_given) {
		janus_config_add_item(config, "media", "ipv6", "true");
	}
	if(args_info.max_nack_queue_given) {
		char mnq[20];
		g_snprintf(mnq, 20, "%d", args_info.max_nack_queue_arg);
		janus_config_add_item(config, "media", "max_nack_queue", mnq);
	}
	if(args_info.no_media_timer_given) {
		char nmt[20];
		g_snprintf(nmt, 20, "%d", args_info.no_media_timer_arg);
		janus_config_add_item(config, "media", "no_media_timer", nmt);
	}
	if(args_info.rfc_4588_given) {
		janus_config_add_item(config, "media", "rfc_4588", "yes");
	}
	if(args_info.rtp_port_range_given) {
		janus_config_add_item(config, "media", "rtp_port_range", args_info.rtp_port_range_arg);
	}
	if(args_info.event_handlers_given) {
		janus_config_add_item(config, "events", "broadcast", "yes");
	}
	janus_config_print(config);

	/* Logging/debugging */
	JANUS_PRINT("Debug/log level is %d\n", janus_log_level);
	janus_config_item *item = janus_config_get_item_drilldown(config, "general", "debug_timestamps");
	if(item && item->value)
		janus_log_timestamps = janus_is_true(item->value);
	JANUS_PRINT("Debug/log timestamps are %s\n", janus_log_timestamps ? "enabled" : "disabled");
	item = janus_config_get_item_drilldown(config, "general", "debug_colors");
	if(item && item->value)
		janus_log_colors = janus_is_true(item->value);
	JANUS_PRINT("Debug/log colors are %s\n", janus_log_colors ? "enabled" : "disabled");

	/* Any IP/interface to enforce/ignore? */
	item = janus_config_get_item_drilldown(config, "nat", "ice_enforce_list");
	if(item && item->value) {
		gchar **list = g_strsplit(item->value, ",", -1);
		gchar *index = list[0];
		if(index != NULL) {
			int i=0;
			while(index != NULL) {
				if(strlen(index) > 0) {
					JANUS_LOG(LOG_INFO, "Adding '%s' to the ICE enforce list...\n", index);
					janus_ice_enforce_interface(g_strdup(index));
				}
				i++;
				index = list[i];
			}
		}
		g_clear_pointer(&list, g_strfreev);
	}
	item = janus_config_get_item_drilldown(config, "nat", "ice_ignore_list");
	if(item && item->value) {
		gchar **list = g_strsplit(item->value, ",", -1);
		gchar *index = list[0];
		if(index != NULL) {
			int i=0;
			while(index != NULL) {
				if(strlen(index) > 0) {
					JANUS_LOG(LOG_INFO, "Adding '%s' to the ICE ignore list...\n", index);
					janus_ice_ignore_interface(g_strdup(index));
				}
				i++;
				index = list[i];
			}
		}
		g_clear_pointer(&list, g_strfreev);
	}
	/* What is the local IP? */
	JANUS_LOG(LOG_VERB, "Selecting local IP address...\n");
	item = janus_config_get_item_drilldown(config, "general", "interface");
	if(item && item->value) {
		JANUS_LOG(LOG_VERB, "  -- Will try to use %s\n", item->value);
		/* Verify that the address is valid */
		struct ifaddrs *ifas = NULL;
		janus_network_address iface;
		janus_network_address_string_buffer ibuf;
		if(getifaddrs(&ifas) || ifas == NULL) {
			JANUS_LOG(LOG_ERR, "Unable to acquire list of network devices/interfaces; some configurations may not work as expected...\n");
		} else {
			if(janus_network_lookup_interface(ifas, item->value, &iface) != 0) {
				JANUS_LOG(LOG_WARN, "Error setting local IP address to %s, falling back to detecting IP address...\n", item->value);
			} else {
				if(janus_network_address_to_string_buffer(&iface, &ibuf) != 0 || janus_network_address_string_buffer_is_null(&ibuf)) {
					JANUS_LOG(LOG_WARN, "Error getting local IP address from %s, falling back to detecting IP address...\n", item->value);
				} else {
					local_ip = g_strdup(janus_network_address_string_from_buffer(&ibuf));
				}
			}
		}
	}
	if(local_ip == NULL) {
		local_ip = janus_network_detect_local_ip_as_string(janus_network_query_options_any_ip);
		if(local_ip == NULL) {
			JANUS_LOG(LOG_WARN, "Couldn't find any address! using 127.0.0.1 as the local IP... (which is NOT going to work out of your machine)\n");
			local_ip = g_strdup("127.0.0.1");
		}
	}
	JANUS_LOG(LOG_INFO, "Using %s as local IP...\n", local_ip);

	/* Was a custom instance name provided? */
	item = janus_config_get_item_drilldown(config, "general", "server_name");
	if(item && item->value) {
		server_name = g_strdup(item->value);
	}

	/* Check if a custom session timeout value was specified */
	item = janus_config_get_item_drilldown(config, "general", "session_timeout");
	if(item && item->value) {
		int st = atoi(item->value);
		if(st < 0) {
			JANUS_LOG(LOG_WARN, "Ignoring session_timeout value as it's not a positive integer\n");
		} else {
			if(st == 0) {
				JANUS_LOG(LOG_WARN, "Session timeouts have been disabled (note, may result in orphaned sessions)\n");
			}
			session_timeout = st;
		}
	}

	/* Is there any API secret to consider? */
	api_secret = NULL;
	item = janus_config_get_item_drilldown(config, "general", "api_secret");
	if(item && item->value) {
		api_secret = g_strdup(item->value);
	}
	/* Is there any API secret to consider? */
	admin_api_secret = NULL;
	item = janus_config_get_item_drilldown(config, "general", "admin_secret");
	if(item && item->value) {
		admin_api_secret = g_strdup(item->value);
	}
	/* Also check if the token based authentication mechanism needs to be enabled */
	item = janus_config_get_item_drilldown(config, "general", "token_auth");
	janus_auth_init(item && item->value && janus_is_true(item->value));

	/* Initialize the recorder code */
	item = janus_config_get_item_drilldown(config, "general", "recordings_tmp_ext");
	if(item && item->value) {
		janus_recorder_init(TRUE, item->value);
	} else {
		janus_recorder_init(FALSE, NULL);
	}

	/* Setup ICE stuff (e.g., checking if the provided STUN server is correct) */
	char *stun_server = NULL, *turn_server = NULL;
	uint16_t stun_port = 0, turn_port = 0;
	char *turn_type = NULL, *turn_user = NULL, *turn_pwd = NULL;
	char *turn_rest_api = NULL, *turn_rest_api_key = NULL;
#ifdef HAVE_LIBCURL
	char *turn_rest_api_method = NULL;
#endif
	const char *nat_1_1_mapping = NULL;
	uint16_t rtp_min_port = 0, rtp_max_port = 0;
	gboolean ice_lite = FALSE, ice_tcp = FALSE, full_trickle = FALSE, ipv6 = FALSE;
	item = janus_config_get_item_drilldown(config, "media", "ipv6");
	ipv6 = (item && item->value) ? janus_is_true(item->value) : FALSE;
	item = janus_config_get_item_drilldown(config, "media", "rtp_port_range");
	if(item && item->value) {
		/* Split in min and max port */
		char *maxport = strrchr(item->value, '-');
		if(maxport != NULL) {
			*maxport = '\0';
			maxport++;
			rtp_min_port = atoi(item->value);
			rtp_max_port = atoi(maxport);
			maxport--;
			*maxport = '-';
		}
		if(rtp_min_port > rtp_max_port) {
			uint16_t temp_port = rtp_min_port;
			rtp_min_port = rtp_max_port;
			rtp_max_port = temp_port;
		}
		if(rtp_max_port == 0)
			rtp_max_port = 65535;
		JANUS_LOG(LOG_INFO, "RTP port range: %u -- %u\n", rtp_min_port, rtp_max_port);
	}
	/* Check if we need to enable the ICE Lite mode */
	item = janus_config_get_item_drilldown(config, "nat", "ice_lite");
	ice_lite = (item && item->value) ? janus_is_true(item->value) : FALSE;
	/* Check if we need to enable ICE-TCP support (warning: still broken, for debugging only) */
	item = janus_config_get_item_drilldown(config, "nat", "ice_tcp");
	ice_tcp = (item && item->value) ? janus_is_true(item->value) : FALSE;
	/* Check if we need to do full-trickle instead of half-trickle */
	item = janus_config_get_item_drilldown(config, "nat", "full_trickle");
	full_trickle = (item && item->value) ? janus_is_true(item->value) : FALSE;
	/* Any STUN server to use in Janus? */
	item = janus_config_get_item_drilldown(config, "nat", "stun_server");
	if(item && item->value)
		stun_server = (char *)item->value;
	item = janus_config_get_item_drilldown(config, "nat", "stun_port");
	if(item && item->value)
		stun_port = atoi(item->value);
	/* Any 1:1 NAT mapping to take into account? */
	item = janus_config_get_item_drilldown(config, "nat", "nat_1_1_mapping");
	if(item && item->value) {
		JANUS_LOG(LOG_VERB, "Using nat_1_1_mapping for public ip - %s\n", item->value);
		if(!janus_network_string_is_valid_address(janus_network_query_options_any_ip, item->value)) {
			JANUS_LOG(LOG_WARN, "Invalid nat_1_1_mapping address %s, disabling...\n", item->value);
		} else {
			nat_1_1_mapping = item->value;
			janus_set_public_ip(item->value);
			janus_ice_enable_nat_1_1();
		}
	}
	/* Any TURN server to use in Janus? */
	item = janus_config_get_item_drilldown(config, "nat", "turn_server");
	if(item && item->value)
		turn_server = (char *)item->value;
	item = janus_config_get_item_drilldown(config, "nat", "turn_port");
	if(item && item->value)
		turn_port = atoi(item->value);
	item = janus_config_get_item_drilldown(config, "nat", "turn_type");
	if(item && item->value)
		turn_type = (char *)item->value;
	item = janus_config_get_item_drilldown(config, "nat", "turn_user");
	if(item && item->value)
		turn_user = (char *)item->value;
	item = janus_config_get_item_drilldown(config, "nat", "turn_pwd");
	if(item && item->value)
		turn_pwd = (char *)item->value;
	/* Check if there's any TURN REST API backend to use */
	item = janus_config_get_item_drilldown(config, "nat", "turn_rest_api");
	if(item && item->value)
		turn_rest_api = (char *)item->value;
	item = janus_config_get_item_drilldown(config, "nat", "turn_rest_api_key");
	if(item && item->value)
		turn_rest_api_key = (char *)item->value;
#ifdef HAVE_LIBCURL
	item = janus_config_get_item_drilldown(config, "nat", "turn_rest_api_method");
	if(item && item->value)
		turn_rest_api_method = (char *)item->value;
#endif
	/* Initialize the ICE stack now */
	janus_ice_init(ice_lite, ice_tcp, full_trickle, ipv6, rtp_min_port, rtp_max_port);
	if(janus_ice_set_stun_server(stun_server, stun_port) < 0) {
		JANUS_LOG(LOG_FATAL, "Invalid STUN address %s:%u\n", stun_server, stun_port);
		exit(1);
	}
	if(janus_ice_set_turn_server(turn_server, turn_port, turn_type, turn_user, turn_pwd) < 0) {
		JANUS_LOG(LOG_FATAL, "Invalid TURN address %s:%u\n", turn_server, turn_port);
		exit(1);
	}
#ifndef HAVE_LIBCURL
	if(turn_rest_api != NULL || turn_rest_api_key != NULL) {
		JANUS_LOG(LOG_WARN, "A TURN REST API backend specified in the settings, but libcurl support has not been built\n");
	}
#else
	if(janus_ice_set_turn_rest_api(turn_rest_api, turn_rest_api_key, turn_rest_api_method) < 0) {
		JANUS_LOG(LOG_FATAL, "Invalid TURN REST API configuration: %s (%s, %s)\n", turn_rest_api, turn_rest_api_key, turn_rest_api_method);
		exit(1);
	}
#endif
	item = janus_config_get_item_drilldown(config, "nat", "nice_debug");
	if(item && item->value && janus_is_true(item->value)) {
		/* Enable libnice debugging */
		janus_ice_debugging_enable();
	}
	if(stun_server == NULL && turn_server == NULL) {
		/* No STUN and TURN server provided for Janus: make sure it isn't on a private address */
		gboolean private_address = FALSE;
		const char *test_ip = nat_1_1_mapping ? nat_1_1_mapping : local_ip;
		janus_network_address addr;
		if(janus_network_string_to_address(janus_network_query_options_any_ip, test_ip, &addr) != 0) {
			JANUS_LOG(LOG_ERR, "Invalid address %s..?\n", test_ip);
		} else {
			if(addr.family == AF_INET) {
				unsigned short int ip[4];
				sscanf(test_ip, "%hu.%hu.%hu.%hu", &ip[0], &ip[1], &ip[2], &ip[3]);
				if(ip[0] == 10) {
					/* Class A private address */
					private_address = TRUE;
				} else if(ip[0] == 172 && (ip[1] >= 16 && ip[1] <= 31)) {
					/* Class B private address */
					private_address = TRUE;
				} else if(ip[0] == 192 && ip[1] == 168) {
					/* Class C private address */
					private_address = TRUE;
				}
			} else {
				/* TODO Similar check for IPv6... */
			}
		}
		if(private_address) {
			JANUS_LOG(LOG_WARN, "Janus is deployed on a private address (%s) but you didn't specify any STUN server!"
			                    " Expect trouble if this is supposed to work over the internet and not just in a LAN...\n", test_ip);
		}
	}
	/* NACK related stuff */
	item = janus_config_get_item_drilldown(config, "media", "max_nack_queue");
	if(item && item->value) {
		int mnq = atoi(item->value);
		if(mnq < 0) {
			JANUS_LOG(LOG_WARN, "Ignoring max_nack_queue value as it's not a positive integer\n");
		} else if(mnq > 0 && mnq < 200) {
			JANUS_LOG(LOG_WARN, "Ignoring max_nack_queue value as it's less than 200\n");
		} else {
			janus_set_max_nack_queue(mnq);
		}
	}
	/* no-media timer */
	item = janus_config_get_item_drilldown(config, "media", "no_media_timer");
	if(item && item->value) {
		int nmt = atoi(item->value);
		if(nmt < 0) {
			JANUS_LOG(LOG_WARN, "Ignoring no_media_timer value as it's not a positive integer\n");
		} else {
			janus_set_no_media_timer(nmt);
		}
	}
	/* RFC4588 support */
	item = janus_config_get_item_drilldown(config, "media", "rfc_4588");
	if(item && item->value) {
		janus_set_rfc4588_enabled(janus_is_true(item->value));
	}

	/* Setup OpenSSL stuff */
	const char* server_pem;
	item = janus_config_get_item_drilldown(config, "certificates", "cert_pem");
	if(!item || !item->value) {
		server_pem = NULL;
	} else {
		server_pem = item->value;
	}

	const char* server_key;
	item = janus_config_get_item_drilldown(config, "certificates", "cert_key");
	if(!item || !item->value) {
		server_key = NULL;
	} else {
		server_key = item->value;
	}
	JANUS_LOG(LOG_VERB, "Using certificates:\n\t%s\n\t%s\n", server_pem, server_key);

	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
	/* ... and DTLS-SRTP in particular */
	if(janus_dtls_srtp_init(server_pem, server_key) < 0) {
		exit(1);
	}
	/* Check if there's any custom value for the starting MTU to use in the BIO filter */
	item = janus_config_get_item_drilldown(config, "media", "dtls_mtu");
	if(item && item->value)
		janus_dtls_bio_filter_set_mtu(atoi(item->value));

#ifdef HAVE_SCTP
	/* Initialize SCTP for DataChannels */
	if(janus_sctp_init() < 0) {
		exit(1);
	}
#else
	JANUS_LOG(LOG_WARN, "Data Channels support not compiled\n");
#endif

	/* Sessions */
	sessions = g_hash_table_new_full(g_int64_hash, g_int64_equal, (GDestroyNotify)g_free, NULL);
	janus_mutex_init(&sessions_mutex);
	/* Start the sessions timeout watchdog */
	sessions_watchdog_context = g_main_context_new();
	GMainLoop *watchdog_loop = g_main_loop_new(sessions_watchdog_context, FALSE);
	GError *error = NULL;
	GThread *watchdog = g_thread_try_new("timeout watchdog", &janus_sessions_watchdog, watchdog_loop, &error);
	if(error != NULL) {
		JANUS_LOG(LOG_FATAL, "Got error %d (%s) trying to start sessions timeout watchdog...\n", error->code, error->message ? error->message : "??");
		exit(1);
	}
	/* Start the thread that will dispatch incoming requests */
	requests = g_async_queue_new_full((GDestroyNotify) janus_request_destroy);
	GThread *requests_thread = g_thread_try_new("sessions requests", &janus_transport_requests, NULL, &error);
	if(error != NULL) {
		JANUS_LOG(LOG_FATAL, "Got error %d (%s) trying to start requests thread...\n", error->code, error->message ? error->message : "??");
		exit(1);
	}
	/* Create a thread pool to handle asynchronous requests, no matter what the transport */
	error = NULL;
	tasks = g_thread_pool_new(janus_transport_task, NULL, -1, FALSE, &error);
	if(error != NULL) {
		/* Something went wrong... */
		JANUS_LOG(LOG_FATAL, "Got error %d (%s) trying to launch the request pool task thread...\n", error->code, error->message ? error->message : "??");
		exit(1);
	}
	/* Wait 120 seconds before stopping idle threads to avoid the creation of too many threads for AddressSanitizer. */
	g_thread_pool_set_max_idle_time(120 * 1000);

	/* Load event handlers */
	const char *path = NULL;
	DIR *dir = NULL;
	/* Event handlers are disabled by default, though: they need to be enabled in the configuration */
	item = janus_config_get_item_drilldown(config, "events", "broadcast");
	gboolean enable_events = FALSE;
	if(item && item->value)
		enable_events = janus_is_true(item->value);
	if(!enable_events) {
		JANUS_LOG(LOG_WARN, "Event handlers support disabled\n");
	} else {
		gchar **disabled_eventhandlers = NULL;
		path = EVENTDIR;
		item = janus_config_get_item_drilldown(config, "general", "events_folder");
		if(item && item->value)
			path = (char *)item->value;
		JANUS_LOG(LOG_INFO, "Event handler plugins folder: %s\n", path);
		dir = opendir(path);
		if(!dir) {
			/* Not really fatal, we don't care and go on anyway: event handlers are not fundamental */
			JANUS_LOG(LOG_FATAL, "\tCouldn't access event handler plugins folder...\n");
		} else {
			item = janus_config_get_item_drilldown(config, "events", "stats_period");
			if(item && item->value) {
				/* Check if we need to use a larger period for pushing statistics to event handlers */
				int period = atoi(item->value);
				if(period < 0) {
					JANUS_LOG(LOG_WARN, "Invalid event handlers statistics period, using default value (1 second)\n");
				} else if(period == 0) {
					janus_ice_set_event_stats_period(0);
					JANUS_LOG(LOG_WARN, "Disabling event handlers statistics period, no media statistics will be pushed to event handlers\n");
				} else {
					janus_ice_set_event_stats_period(period);
					JANUS_LOG(LOG_INFO, "Setting event handlers statistics period to %d seconds\n", period);
				}
			}
			/* Any event handlers to ignore? */
			item = janus_config_get_item_drilldown(config, "events", "disable");
			if(item && item->value)
				disabled_eventhandlers = g_strsplit(item->value, ",", -1);
			/* Open the shared objects */
			struct dirent *eventent = NULL;
			char eventpath[1024];
			while((eventent = readdir(dir))) {
				int len = strlen(eventent->d_name);
				if (len < 4) {
					continue;
				}
				if (strcasecmp(eventent->d_name+len-strlen(SHLIB_EXT), SHLIB_EXT)) {
					continue;
				}
				/* Check if this event handler has been disabled in the configuration file */
				if(disabled_eventhandlers != NULL) {
					gchar *index = disabled_eventhandlers[0];
					if(index != NULL) {
						int i=0;
						gboolean skip = FALSE;
						while(index != NULL) {
							while(isspace(*index))
								index++;
							if(strlen(index) && !strcmp(index, eventent->d_name)) {
								JANUS_LOG(LOG_WARN, "Event handler plugin '%s' has been disabled, skipping...\n", eventent->d_name);
								skip = TRUE;
								break;
							}
							i++;
							index = disabled_eventhandlers[i];
						}
						if(skip)
							continue;
					}
				}
				JANUS_LOG(LOG_INFO, "Loading event handler plugin '%s'...\n", eventent->d_name);
				memset(eventpath, 0, 1024);
				g_snprintf(eventpath, 1024, "%s/%s", path, eventent->d_name);
				void *event = dlopen(eventpath, RTLD_NOW | RTLD_GLOBAL);
				if (!event) {
					JANUS_LOG(LOG_ERR, "\tCouldn't load event handler plugin '%s': %s\n", eventent->d_name, dlerror());
				} else {
					create_e *create = (create_e*) dlsym(event, "create");
					const char *dlsym_error = dlerror();
					if (dlsym_error) {
						JANUS_LOG(LOG_ERR, "\tCouldn't load symbol 'create': %s\n", dlsym_error);
						continue;
					}
					janus_eventhandler *janus_eventhandler = create();
					if(!janus_eventhandler) {
						JANUS_LOG(LOG_ERR, "\tCouldn't use function 'create'...\n");
						continue;
					}
					/* Are all the mandatory methods and callbacks implemented? */
					if(!janus_eventhandler->init || !janus_eventhandler->destroy ||
							!janus_eventhandler->get_api_compatibility ||
							!janus_eventhandler->get_version ||
							!janus_eventhandler->get_version_string ||
							!janus_eventhandler->get_description ||
							!janus_eventhandler->get_package ||
							!janus_eventhandler->get_name ||
							!janus_eventhandler->incoming_event) {
						JANUS_LOG(LOG_ERR, "\tMissing some mandatory methods/callbacks, skipping this event handler plugin...\n");
						continue;
					}
					if(janus_eventhandler->get_api_compatibility() < JANUS_EVENTHANDLER_API_VERSION) {
						JANUS_LOG(LOG_ERR, "The '%s' event handler plugin was compiled against an older version of the API (%d < %d), skipping it: update it to enable it again\n",
							janus_eventhandler->get_package(), janus_eventhandler->get_api_compatibility(), JANUS_EVENTHANDLER_API_VERSION);
						continue;
					}
					janus_eventhandler->init(configs_folder);
					JANUS_LOG(LOG_VERB, "\tVersion: %d (%s)\n", janus_eventhandler->get_version(), janus_eventhandler->get_version_string());
					JANUS_LOG(LOG_VERB, "\t   [%s] %s\n", janus_eventhandler->get_package(), janus_eventhandler->get_name());
					JANUS_LOG(LOG_VERB, "\t   %s\n", janus_eventhandler->get_description());
					JANUS_LOG(LOG_VERB, "\t   Plugin API version: %d\n", janus_eventhandler->get_api_compatibility());
					JANUS_LOG(LOG_VERB, "\t   Subscriptions:");
					if(janus_eventhandler->events_mask == 0) {
						JANUS_LOG(LOG_VERB, " none");
					} else {
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_SESSION))
							JANUS_LOG(LOG_VERB, " sessions");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_HANDLE))
							JANUS_LOG(LOG_VERB, " handles");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_JSEP))
							JANUS_LOG(LOG_VERB, " jsep");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_WEBRTC))
							JANUS_LOG(LOG_VERB, " webrtc");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_MEDIA))
							JANUS_LOG(LOG_VERB, " media");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_PLUGIN))
							JANUS_LOG(LOG_VERB, " plugins");
						if(janus_flags_is_set(&janus_eventhandler->events_mask, JANUS_EVENT_TYPE_TRANSPORT))
							JANUS_LOG(LOG_VERB, " transports");
					}
					JANUS_LOG(LOG_VERB, "\n");
					if(eventhandlers == NULL)
						eventhandlers = g_hash_table_new(g_str_hash, g_str_equal);
					g_hash_table_insert(eventhandlers, (gpointer)janus_eventhandler->get_package(), janus_eventhandler);
					if(eventhandlers_so == NULL)
						eventhandlers_so = g_hash_table_new(g_str_hash, g_str_equal);
					g_hash_table_insert(eventhandlers_so, (gpointer)janus_eventhandler->get_package(), event);
				}
			}
		}
		closedir(dir);
		if(disabled_eventhandlers != NULL)
			g_strfreev(disabled_eventhandlers);
		disabled_eventhandlers = NULL;
		/* Initialize the event broadcaster */
		if(janus_events_init(enable_events, (server_name ? server_name : (char *)JANUS_SERVER_NAME), eventhandlers) < 0) {
			JANUS_LOG(LOG_FATAL, "Error initializing the Event handlers mechanism...\n");
			exit(1);
		}
	}

	/* Load plugins */
	path = PLUGINDIR;
	item = janus_config_get_item_drilldown(config, "general", "plugins_folder");
	if(item && item->value)
		path = (char *)item->value;
	JANUS_LOG(LOG_INFO, "Plugins folder: %s\n", path);
	dir = opendir(path);
	if(!dir) {
		JANUS_LOG(LOG_FATAL, "\tCouldn't access plugins folder...\n");
		exit(1);
	}
	/* Any plugin to ignore? */
	gchar **disabled_plugins = NULL;
	item = janus_config_get_item_drilldown(config, "plugins", "disable");
	if(item && item->value)
		disabled_plugins = g_strsplit(item->value, ",", -1);
	/* Open the shared objects */
	struct dirent *pluginent = NULL;
	char pluginpath[1024];
	while((pluginent = readdir(dir))) {
		int len = strlen(pluginent->d_name);
		if (len < 4) {
			continue;
		}
		if (strcasecmp(pluginent->d_name+len-strlen(SHLIB_EXT), SHLIB_EXT)) {
			continue;
		}
		/* Check if this plugins has been disabled in the configuration file */
		if(disabled_plugins != NULL) {
			gchar *index = disabled_plugins[0];
			if(index != NULL) {
				int i=0;
				gboolean skip = FALSE;
				while(index != NULL) {
					while(isspace(*index))
						index++;
					if(strlen(index) && !strcmp(index, pluginent->d_name)) {
						JANUS_LOG(LOG_WARN, "Plugin '%s' has been disabled, skipping...\n", pluginent->d_name);
						skip = TRUE;
						break;
					}
					i++;
					index = disabled_plugins[i];
				}
				if(skip)
					continue;
			}
		}
		JANUS_LOG(LOG_INFO, "Loading plugin '%s'...\n", pluginent->d_name);
		memset(pluginpath, 0, 1024);
		g_snprintf(pluginpath, 1024, "%s/%s", path, pluginent->d_name);
		void *plugin = dlopen(pluginpath, RTLD_NOW | RTLD_GLOBAL);
		if (!plugin) {
			JANUS_LOG(LOG_ERR, "\tCouldn't load plugin '%s': %s\n", pluginent->d_name, dlerror());
		} else {
			create_p *create = (create_p*) dlsym(plugin, "create");
			const char *dlsym_error = dlerror();
			if (dlsym_error) {
				JANUS_LOG(LOG_ERR, "\tCouldn't load symbol 'create': %s\n", dlsym_error);
				continue;
			}
			janus_plugin *janus_plugin = create();
			if(!janus_plugin) {
				JANUS_LOG(LOG_ERR, "\tCouldn't use function 'create'...\n");
				continue;
			}
			/* Are all the mandatory methods and callbacks implemented? */
			if(!janus_plugin->init || !janus_plugin->destroy ||
					!janus_plugin->get_api_compatibility ||
					!janus_plugin->get_version ||
					!janus_plugin->get_version_string ||
					!janus_plugin->get_description ||
					!janus_plugin->get_package ||
					!janus_plugin->get_name ||
					!janus_plugin->create_session ||
					!janus_plugin->query_session ||
					!janus_plugin->destroy_session ||
					!janus_plugin->handle_message ||
					!janus_plugin->setup_media ||
					!janus_plugin->hangup_media) {
				JANUS_LOG(LOG_ERR, "\tMissing some mandatory methods/callbacks, skipping this plugin...\n");
				continue;
			}
			if(janus_plugin->get_api_compatibility() < JANUS_PLUGIN_API_VERSION) {
				JANUS_LOG(LOG_ERR, "The '%s' plugin was compiled against an older version of the API (%d < %d), skipping it: update it to enable it again\n",
					janus_plugin->get_package(), janus_plugin->get_api_compatibility(), JANUS_PLUGIN_API_VERSION);
				continue;
			}
			if(janus_plugin->init(&janus_handler_plugin, configs_folder) < 0) {
				JANUS_LOG(LOG_WARN, "The '%s' plugin could not be initialized\n", janus_plugin->get_package());
				dlclose(plugin);
				continue;
			}
			JANUS_LOG(LOG_VERB, "\tVersion: %d (%s)\n", janus_plugin->get_version(), janus_plugin->get_version_string());
			JANUS_LOG(LOG_VERB, "\t   [%s] %s\n", janus_plugin->get_package(), janus_plugin->get_name());
			JANUS_LOG(LOG_VERB, "\t   %s\n", janus_plugin->get_description());
			JANUS_LOG(LOG_VERB, "\t   Plugin API version: %d\n", janus_plugin->get_api_compatibility());
			if(!janus_plugin->incoming_rtp && !janus_plugin->incoming_rtcp && !janus_plugin->incoming_data) {
				JANUS_LOG(LOG_WARN, "The '%s' plugin doesn't implement any callback for RTP/RTCP/data... is this on purpose?\n",
					janus_plugin->get_package());
			}
			if(!janus_plugin->incoming_rtp && !janus_plugin->incoming_rtcp && janus_plugin->incoming_data) {
				JANUS_LOG(LOG_WARN, "The '%s' plugin will only handle data channels (no RTP/RTCP)... is this on purpose?\n",
					janus_plugin->get_package());
			}
			if(plugins == NULL)
				plugins = g_hash_table_new(g_str_hash, g_str_equal);
			g_hash_table_insert(plugins, (gpointer)janus_plugin->get_package(), janus_plugin);
			if(plugins_so == NULL)
				plugins_so = g_hash_table_new(g_str_hash, g_str_equal);
			g_hash_table_insert(plugins_so, (gpointer)janus_plugin->get_package(), plugin);
		}
	}
	closedir(dir);
	if(disabled_plugins != NULL)
		g_strfreev(disabled_plugins);
	disabled_plugins = NULL;

	/* Load transports */
	gboolean janus_api_enabled = FALSE, admin_api_enabled = FALSE;
	path = TRANSPORTDIR;
	item = janus_config_get_item_drilldown(config, "general", "transports_folder");
	if(item && item->value)
		path = (char *)item->value;
	JANUS_LOG(LOG_INFO, "Transport plugins folder: %s\n", path);
	dir = opendir(path);
	if(!dir) {
		JANUS_LOG(LOG_FATAL, "\tCouldn't access transport plugins folder...\n");
		exit(1);
	}
	/* Any transport to ignore? */
	gchar **disabled_transports = NULL;
	item = janus_config_get_item_drilldown(config, "transports", "disable");
	if(item && item->value)
		disabled_transports = g_strsplit(item->value, ",", -1);
	/* Open the shared objects */
	struct dirent *transportent = NULL;
	char transportpath[1024];
	while((transportent = readdir(dir))) {
		int len = strlen(transportent->d_name);
		if (len < 4) {
			continue;
		}
		if (strcasecmp(transportent->d_name+len-strlen(SHLIB_EXT), SHLIB_EXT)) {
			continue;
		}
		/* Check if this transports has been disabled in the configuration file */
		if(disabled_transports != NULL) {
			gchar *index = disabled_transports[0];
			if(index != NULL) {
				int i=0;
				gboolean skip = FALSE;
				while(index != NULL) {
					while(isspace(*index))
						index++;
					if(strlen(index) && !strcmp(index, transportent->d_name)) {
						JANUS_LOG(LOG_WARN, "Transport plugin '%s' has been disabled, skipping...\n", transportent->d_name);
						skip = TRUE;
						break;
					}
					i++;
					index = disabled_transports[i];
				}
				if(skip)
					continue;
			}
		}
		JANUS_LOG(LOG_INFO, "Loading transport plugin '%s'...\n", transportent->d_name);
		memset(transportpath, 0, 1024);
		g_snprintf(transportpath, 1024, "%s/%s", path, transportent->d_name);
		void *transport = dlopen(transportpath, RTLD_NOW | RTLD_GLOBAL);
		if (!transport) {
			JANUS_LOG(LOG_ERR, "\tCouldn't load transport plugin '%s': %s\n", transportent->d_name, dlerror());
		} else {
			create_t *create = (create_t*) dlsym(transport, "create");
			const char *dlsym_error = dlerror();
			if (dlsym_error) {
				JANUS_LOG(LOG_ERR, "\tCouldn't load symbol 'create': %s\n", dlsym_error);
				continue;
			}
			janus_transport *janus_transport = create();
			if(!janus_transport) {
				JANUS_LOG(LOG_ERR, "\tCouldn't use function 'create'...\n");
				continue;
			}
			/* Are all the mandatory methods and callbacks implemented? */
			if(!janus_transport->init || !janus_transport->destroy ||
					!janus_transport->get_api_compatibility ||
					!janus_transport->get_version ||
					!janus_transport->get_version_string ||
					!janus_transport->get_description ||
					!janus_transport->get_package ||
					!janus_transport->get_name ||
					!janus_transport->send_message ||
					!janus_transport->is_janus_api_enabled ||
					!janus_transport->is_admin_api_enabled ||
					!janus_transport->session_created ||
					!janus_transport->session_over) {
				JANUS_LOG(LOG_ERR, "\tMissing some mandatory methods/callbacks, skipping this transport plugin...\n");
				continue;
			}
			if(janus_transport->get_api_compatibility() < JANUS_TRANSPORT_API_VERSION) {
				JANUS_LOG(LOG_ERR, "The '%s' transport plugin was compiled against an older version of the API (%d < %d), skipping it: update it to enable it again\n",
					janus_transport->get_package(), janus_transport->get_api_compatibility(), JANUS_TRANSPORT_API_VERSION);
				continue;
			}
			if(janus_transport->init(&janus_handler_transport, configs_folder) < 0) {
				JANUS_LOG(LOG_WARN, "The '%s' plugin could not be initialized\n", janus_transport->get_package());
				dlclose(transport);
				continue;
			}
			JANUS_LOG(LOG_VERB, "\tVersion: %d (%s)\n", janus_transport->get_version(), janus_transport->get_version_string());
			JANUS_LOG(LOG_VERB, "\t   [%s] %s\n", janus_transport->get_package(), janus_transport->get_name());
			JANUS_LOG(LOG_VERB, "\t   %s\n", janus_transport->get_description());
			JANUS_LOG(LOG_VERB, "\t   Plugin API version: %d\n", janus_transport->get_api_compatibility());
			JANUS_LOG(LOG_VERB, "\t   Janus API: %s\n", janus_transport->is_janus_api_enabled() ? "enabled" : "disabled");
			JANUS_LOG(LOG_VERB, "\t   Admin API: %s\n", janus_transport->is_admin_api_enabled() ? "enabled" : "disabled");
			janus_api_enabled = janus_api_enabled || janus_transport->is_janus_api_enabled();
			admin_api_enabled = admin_api_enabled || janus_transport->is_admin_api_enabled();
			if(transports == NULL)
				transports = g_hash_table_new(g_str_hash, g_str_equal);
			g_hash_table_insert(transports, (gpointer)janus_transport->get_package(), janus_transport);
			if(transports_so == NULL)
				transports_so = g_hash_table_new(g_str_hash, g_str_equal);
			g_hash_table_insert(transports_so, (gpointer)janus_transport->get_package(), transport);
		}
	}
	closedir(dir);
	if(disabled_transports != NULL)
		g_strfreev(disabled_transports);
	disabled_transports = NULL;
	/* Make sure at least a Janus API transport is available */
	if(!janus_api_enabled) {
		JANUS_LOG(LOG_FATAL, "No Janus API transport is available... enable at least one and restart Janus\n");
		exit(1);	/* FIXME Should we really give up? */
	}
	/* Make sure at least an admin API transport is available, if the auth mechanism is enabled */
	if(!admin_api_enabled && janus_auth_is_enabled()) {
		JANUS_LOG(LOG_FATAL, "No Admin/monitor transport is available, but the token based authentication mechanism is enabled... this will cause all requests to fail, giving up! If you want to use tokens, enable the Admin/monitor API and restart Janus\n");
		exit(1);	/* FIXME Should we really give up? */
	}

	/* Ok, Janus has started! Let the parent now about this if we're daemonizing */
	if(daemonize) {
		int code = 0;
		ssize_t res = 0;
		do {
			res = write(pipefd[1], &code, sizeof(int));
		} while(res == -1 && errno == EINTR);
	}

	/* If the Event Handlers mechanism is enabled, notify handlers that Janus just started */
	if(janus_events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "status", json_string("started"));
		json_object_set_new(info, "info", janus_info(NULL));
		janus_events_notify_handlers(JANUS_EVENT_TYPE_CORE, 0, info);
	}

	while(!g_atomic_int_get(&stop)) {
		/* Loop until we have to stop */
		usleep(250000); /* A signal will cancel usleep() but not g_usleep() */
	}

	/* If the Event Handlers mechanism is enabled, notify handlers that Janus is hanging up */
	if(janus_events_is_enabled()) {
		json_t *info = json_object();
		json_object_set_new(info, "status", json_string("shutdown"));
		json_object_set_new(info, "signum", json_integer(stop_signal));
		janus_events_notify_handlers(JANUS_EVENT_TYPE_CORE, 0, info);
	}

	/* Done */
	JANUS_LOG(LOG_INFO, "Ending sessions timeout watchdog...\n");
	g_main_loop_quit(watchdog_loop);
	g_thread_join(watchdog);
	watchdog = NULL;
	g_main_loop_unref(watchdog_loop);
	g_main_context_unref(sessions_watchdog_context);
	sessions_watchdog_context = NULL;

	if(config)
		janus_config_destroy(config);

	JANUS_LOG(LOG_INFO, "Closing transport plugins:\n");
	if(transports != NULL && g_hash_table_size(transports) > 0) {
		g_hash_table_foreach(transports, janus_transport_close, NULL);
		g_hash_table_destroy(transports);
	}
	if(transports_so != NULL && g_hash_table_size(transports_so) > 0) {
		g_hash_table_foreach(transports_so, janus_transportso_close, NULL);
		g_hash_table_destroy(transports_so);
	}
	/* Get rid of requests tasks and thread too */
	g_thread_pool_free(tasks, FALSE, FALSE);
	JANUS_LOG(LOG_INFO, "Ending requests thread...\n");
	g_async_queue_push(requests, &exit_message);
	g_thread_join(requests_thread);
	requests_thread = NULL;
	g_async_queue_unref(requests);

	JANUS_LOG(LOG_INFO, "Destroying sessions...\n");
	g_clear_pointer(&sessions, g_hash_table_destroy);
	janus_ice_deinit();
	JANUS_LOG(LOG_INFO, "Freeing crypto resources...\n");
	janus_dtls_srtp_cleanup();
	EVP_cleanup();
	ERR_free_strings();
#ifdef HAVE_SCTP
	JANUS_LOG(LOG_INFO, "De-initializing SCTP...\n");
	janus_sctp_deinit();
#endif
	janus_auth_deinit();

	JANUS_LOG(LOG_INFO, "Closing plugins:\n");
	if(plugins != NULL && g_hash_table_size(plugins) > 0) {
		g_hash_table_foreach(plugins, janus_plugin_close, NULL);
		g_hash_table_destroy(plugins);
	}
	if(plugins_so != NULL && g_hash_table_size(plugins_so) > 0) {
		g_hash_table_foreach(plugins_so, janus_pluginso_close, NULL);
		g_hash_table_destroy(plugins_so);
	}

	JANUS_LOG(LOG_INFO, "Closing event handlers:\n");
	janus_events_deinit();
	if(eventhandlers != NULL && g_hash_table_size(eventhandlers) > 0) {
		g_hash_table_foreach(eventhandlers, janus_eventhandler_close, NULL);
		g_hash_table_destroy(eventhandlers);
	}
	if(eventhandlers_so != NULL && g_hash_table_size(eventhandlers_so) > 0) {
		g_hash_table_foreach(eventhandlers_so, janus_eventhandlerso_close, NULL);
		g_hash_table_destroy(eventhandlers_so);
	}

	janus_recorder_deinit();
	g_free(local_ip);

#ifdef REFCOUNT_DEBUG
	/* Any reference counters that are still up while we're leaving? (debug-mode only) */
	janus_mutex_lock(&counters_mutex);
	if(counters && g_hash_table_size(counters) > 0) {
		JANUS_PRINT("Debugging reference counters: %d still allocated\n", g_hash_table_size(counters));
		GHashTableIter iter;
		gpointer value;
		g_hash_table_iter_init(&iter, counters);
		while(g_hash_table_iter_next(&iter, NULL, &value)) {
			JANUS_PRINT("  -- %p\n", value);
		}
	} else {
		JANUS_PRINT("Debugging reference counters: 0 still allocated\n");
	}
	janus_mutex_unlock(&counters_mutex);
#endif

	JANUS_PRINT("Bye!\n");

	exit(0);
}
