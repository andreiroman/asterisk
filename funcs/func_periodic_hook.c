/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2014, Russell Bryant
 *
 * Russell Bryant <russell@russellbryant.net>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Periodic dialplan hooks.
 *
 * \author Russell Bryant <russell@russellbryant.net>
 *
 * \ingroup functions
 */

/*** MODULEINFO
	<support_level>core</support_level>
	<depend>app_chanspy</depend>
	<depend>func_cut</depend>
	<depend>func_groupcount</depend>
	<depend>func_uri</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/app.h"
#include "asterisk/audiohook.h"
#define AST_API_MODULE
#include "asterisk/beep.h"

/*** DOCUMENTATION
	<function name="PERIODIC_HOOK" language="en_US">
		<synopsis>
			Execute a periodic dialplan hook into the audio of a call.
		</synopsis>
		<syntax>
			<parameter name="context" required="true">
				<para>(On Read Only) Context for the hook extension.</para>
			</parameter>
			<parameter name="extension" required="true">
				<para>(On Read Only) The hook extension.</para>
			</parameter>
			<parameter name="interval" required="true">
				<para>(On Read Only) Number of seconds in between hook runs.
				Whole seconds only.</para>
			</parameter>
			<parameter name="hook_id" required="true">
				<para>(On Write Only) The hook ID.</para>
			</parameter>
		</syntax>
		<description>
			<para>For example, you could use this function to enable playing
			a periodic <literal>beep</literal> sound in a call.</para>
			<para/>
			<para>To turn on:</para>
			<para>  Set(BEEPID=${PERIODIC_HOOK(hooks,beep,180)})</para>
			<para/>
			<para>To turn off:</para>
			<para>  Set(PERIODIC_HOOK(${BEEPID})=off)</para>
			<para/>
			<para>To turn back on again later:</para>
			<para>Set(PERIODIC_HOOK(${BEEPID})=on)</para>
			<para/>
			<para>It is important to note that the hook does not actually
			run on the channel itself.  It runs asynchronously on a new channel.
			Any audio generated by the hook gets injected into the call for
			the channel PERIODIC_HOOK() was set on.</para>
			<para/>
			<para>The hook dialplan will have two variables available.
			<variable>HOOK_CHANNEL</variable> is the channel the hook is
			enabled on.  <variable>HOOK_ID</variable> is the hook ID for
			enabling or disabling the hook.</para>
		</description>
	</function>
 ***/

static const char context_name[] = "__func_periodic_hook_context__";
static const char exten_name[] = "hook";
static const char full_exten_name[] = "hook@__func_periodic_hook_context__";

static const char beep_exten[] = "beep";

/*!
 * \brief Last used hook ID
 *
 * This is incremented each time a hook is created to give each hook a unique
 * ID.
 */
static unsigned int global_hook_id;

/*! State put in a datastore to track the state of the hook */
struct hook_state {
	/*!
	 * \brief audiohook used as a callback into this module
	 *
	 * \note The code assumes this is the first element in the struct
	 */
	struct ast_audiohook audiohook;
	/*! Seconds between each hook run */
	unsigned int interval;
	/*! The last time the hook ran */
	struct timeval last_hook;
	/*! Dialplan context for the hook */
	char *context;
	/*! Dialplan extension for the hook */
	char *exten;
	/*! Hook ID */
	unsigned int hook_id;
	/*! Non-zero if the hook is currently disabled */
	unsigned char disabled;
};

static void hook_datastore_destroy_callback(void *data)
{
	struct hook_state *state = data;

	ast_audiohook_lock(&state->audiohook);
	ast_audiohook_detach(&state->audiohook);
	ast_audiohook_unlock(&state->audiohook);
	ast_audiohook_destroy(&state->audiohook);

	ast_free(state->context);
	ast_free(state->exten);
	ast_free(state);

	ast_module_unref(ast_module_info->self);
}

static const struct ast_datastore_info hook_datastore = {
	.type = AST_MODULE,
	.destroy = hook_datastore_destroy_callback,
};

/*! Arguments to the thread that launches the hook */
struct hook_thread_arg {
	/*! Hook ID */
	char *hook_id;
	/*! Name of the channel the hook was set on */
	char *chan_name;
	/*! Dialplan context for the hook */
	char *context;
	/*! Dialplan extension for the hook */
	char *exten;
};

static void hook_thread_arg_destroy(struct hook_thread_arg *arg)
{
	ast_free(arg->hook_id);
	ast_free(arg->chan_name);
	ast_free(arg->context);
	ast_free(arg->exten);
	ast_free(arg);
}

static void *hook_launch_thread(void *data)
{
	struct hook_thread_arg *arg = data;
	struct ast_variable hook_id = {
		.name = "HOOK_ID",
		.value = arg->hook_id,
	};
	struct ast_variable chan_name_var = {
		.name = "HOOK_CHANNEL",
		.value = arg->chan_name,
		.next = &hook_id,
	};

	ast_pbx_outgoing_exten("Local", NULL, full_exten_name, 60,
			arg->context, arg->exten, 1, NULL, 0, NULL, NULL, &chan_name_var,
			NULL, NULL, 1, NULL);

	hook_thread_arg_destroy(arg);

	return NULL;
}

static struct hook_thread_arg *hook_thread_arg_alloc(struct ast_channel *chan,
		struct hook_state *state)
{
	struct hook_thread_arg *arg;

	if (!(arg = ast_calloc(1, sizeof(*arg)))) {
		return NULL;
	}

	ast_channel_lock(chan);
	arg->chan_name = ast_strdup(ast_channel_name(chan));
	ast_channel_unlock(chan);
	if (!arg->chan_name) {
		hook_thread_arg_destroy(arg);
		return NULL;
	}

	if (ast_asprintf(&arg->hook_id, "%u", state->hook_id) == -1) {
		hook_thread_arg_destroy(arg);
		return NULL;
	}

	if (!(arg->context = ast_strdup(state->context))) {
		hook_thread_arg_destroy(arg);
		return NULL;
	}

	if (!(arg->exten = ast_strdup(state->exten))) {
		hook_thread_arg_destroy(arg);
		return NULL;
	}

	return arg;
}

static int do_hook(struct ast_channel *chan, struct hook_state *state)
{
	pthread_t t;
	struct hook_thread_arg *arg;
	int res;

	if (!(arg = hook_thread_arg_alloc(chan, state))) {
		return -1;
	}

	/*
	 * We don't want to block normal frame processing *at all* while we kick
	 * this off, so do it in a new thread.
	 */
	res = ast_pthread_create_detached_background(&t, NULL, hook_launch_thread, arg);
	if (res != 0) {
		hook_thread_arg_destroy(arg);
	}

	return res;
}

static int hook_callback(struct ast_audiohook *audiohook, struct ast_channel *chan,
		struct ast_frame *frame, enum ast_audiohook_direction direction)
{
	struct hook_state *state = (struct hook_state *) audiohook; /* trust me. */
	struct timeval now;
	int res = 0;

	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE || state->disabled) {
		return 0;
	}

	now = ast_tvnow();
	if (ast_tvdiff_ms(now, state->last_hook) > state->interval * 1000) {
		if ((res = do_hook(chan, state))) {
			const char *name;
			ast_channel_lock(chan);
			name = ast_strdupa(ast_channel_name(chan));
			ast_channel_unlock(chan);
			ast_log(LOG_WARNING, "Failed to run hook on '%s'\n", name);
		}
		state->last_hook = now;
	}

	return res;
}

static struct hook_state *hook_state_alloc(const char *context, const char *exten,
		unsigned int interval, unsigned int hook_id)
{
	struct hook_state *state;

	if (!(state = ast_calloc(1, sizeof(*state)))) {
		return NULL;
	}

	state->context = ast_strdup(context);
	state->exten = ast_strdup(exten);
	state->interval = interval;
	state->hook_id = hook_id;

	ast_audiohook_init(&state->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE,
			AST_MODULE, AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
	state->audiohook.manipulate_callback = hook_callback;

	return state;
}

static int init_hook(struct ast_channel *chan, const char *context, const char *exten,
		unsigned int interval, unsigned int hook_id)
{
	struct hook_state *state;
	struct ast_datastore *datastore;
	char uid[32];

	snprintf(uid, sizeof(uid), "%u", hook_id);

	if (!(datastore = ast_datastore_alloc(&hook_datastore, uid))) {
		return -1;
	}
	ast_module_ref(ast_module_info->self);
	if (!(state = hook_state_alloc(context, exten, interval, hook_id))) {
		ast_datastore_free(datastore);
		return -1;
	}
	datastore->data = state;

	ast_channel_lock(chan);
	ast_channel_datastore_add(chan, datastore);
	ast_audiohook_attach(chan, &state->audiohook);
	ast_channel_unlock(chan);

	return 0;
}

static int hook_on(struct ast_channel *chan, const char *data, unsigned int hook_id)
{
	char *parse = ast_strdupa(S_OR(data, ""));
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(context);
		AST_APP_ARG(exten);
		AST_APP_ARG(interval);
	);
	unsigned int interval;

	AST_STANDARD_APP_ARGS(args, parse);

	if (ast_strlen_zero(args.interval) ||
			sscanf(args.interval, "%30u", &interval) != 1 || interval == 0) {
		ast_log(LOG_WARNING, "Invalid hook interval: '%s'\n", S_OR(args.interval, ""));
		return -1;
	}

	if (ast_strlen_zero(args.context) || ast_strlen_zero(args.exten)) {
		ast_log(LOG_WARNING, "A context and extension are required for PERIODIC_HOOK().\n");
		return -1;
	}

	ast_debug(1, "hook to %s@%s enabled on %s with interval of %u seconds\n",
			args.exten, args.context, ast_channel_name(chan), interval);

	return init_hook(chan, args.context, args.exten, interval, hook_id);
}

static int hook_off(struct ast_channel *chan, const char *hook_id)
{
	struct ast_datastore *datastore;
	struct hook_state *state;

	if (ast_strlen_zero(hook_id)) {
		return -1;
	}

	ast_channel_lock(chan);

	if (!(datastore = ast_channel_datastore_find(chan, &hook_datastore, hook_id))) {
		ast_log(LOG_WARNING, "Hook with ID '%s' not found on channel '%s'\n", hook_id,
				ast_channel_name(chan));
		ast_channel_unlock(chan);
		return -1;
	}

	state = datastore->data;
	state->disabled = 1;

	ast_channel_unlock(chan);

	return 0;
}

static int hook_read(struct ast_channel *chan, const char *cmd, char *data,
	       char *buf, size_t len)
{
	unsigned int hook_id;

	if (!chan) {
		return -1;
	}

	hook_id = (unsigned int) ast_atomic_fetchadd_int((int *) &global_hook_id, 1);

	snprintf(buf, len, "%u", hook_id);

	return hook_on(chan, data, hook_id);
}

static int hook_re_enable(struct ast_channel *chan, const char *uid)
{
	struct ast_datastore *datastore;
	struct hook_state *state;

	if (ast_strlen_zero(uid)) {
		return -1;
	}

	ast_channel_lock(chan);

	if (!(datastore = ast_channel_datastore_find(chan, &hook_datastore, uid))) {
		ast_log(LOG_WARNING, "Hook with ID '%s' not found on '%s'\n",
				uid, ast_channel_name(chan));
		ast_channel_unlock(chan);
		return -1;
	}

	state = datastore->data;
	state->disabled = 0;

	ast_channel_unlock(chan);

	return 0;
}

static int hook_write(struct ast_channel *chan, const char *cmd, char *data,
		const char *value)
{
	int res;

	if (!chan) {
		return -1;
	}

	if (ast_false(value)) {
		res = hook_off(chan, data);
	} else if (ast_true(value)) {
		res = hook_re_enable(chan, data);
	} else {
		ast_log(LOG_WARNING, "Invalid value for PERIODIC_HOOK function: '%s'\n", value);
		res = -1;
	}

	return res;
}

static struct ast_custom_function hook_function = {
	.name = "PERIODIC_HOOK",
	.read = hook_read,
	.write = hook_write,
};

static struct ast_context *func_periodic_hook_context;

static int unload_module(void)
{
	if (func_periodic_hook_context) {
		ast_context_destroy(func_periodic_hook_context, AST_MODULE);
	}

	return ast_custom_function_unregister(&hook_function);
}

static int load_module(void)
{
	int res;

	func_periodic_hook_context = ast_context_find_or_create(NULL, NULL,
			context_name, AST_MODULE);
	if (!func_periodic_hook_context) {
		ast_log(LOG_ERROR, "Failed to create %s dialplan context.\n", context_name);
		return AST_MODULE_LOAD_DECLINE;
	}

	/*
	 * Based on a handy recipe from the Asterisk Cookbook.
	 */
	ast_add_extension(context_name, 1, exten_name, 1, "", "",
			"Set", "EncodedChannel=${CUT(HOOK_CHANNEL,-,1-2)}",
			NULL, AST_MODULE);
	ast_add_extension(context_name, 1, exten_name, 2, "", "",
			"Set", "GROUP_NAME=${EncodedChannel}${HOOK_ID}",
			NULL, AST_MODULE);
	ast_add_extension(context_name, 1, exten_name, 3, "", "",
			"Set", "GROUP(periodic-hook)=${GROUP_NAME}",
			NULL, AST_MODULE);
	ast_add_extension(context_name, 1, exten_name, 4, "", "", "ExecIf",
			"$[${GROUP_COUNT(${GROUP_NAME}@periodic-hook)} > 1]?Hangup()",
			NULL, AST_MODULE);
	ast_add_extension(context_name, 1, exten_name, 5, "", "",
			"Set", "ChannelToSpy=${URIDECODE(${EncodedChannel})}",
			NULL, AST_MODULE);
	ast_add_extension(context_name, 1, exten_name, 6, "", "",
			"ChanSpy", "${ChannelToSpy},qEB", NULL, AST_MODULE);

	res = ast_add_extension(context_name, 1, beep_exten, 1, "", "",
			"Answer", "", NULL, AST_MODULE);
	res |= ast_add_extension(context_name, 1, beep_exten, 2, "", "",
			"Playback", "beep", NULL, AST_MODULE);

	res = ast_custom_function_register_escalating(&hook_function, AST_CFE_BOTH);

	return res ? AST_MODULE_LOAD_DECLINE : AST_MODULE_LOAD_SUCCESS;
}

int AST_OPTIONAL_API_NAME(ast_beep_start)(struct ast_channel *chan,
		unsigned int interval, char *beep_id, size_t len)
{
	char args[AST_MAX_EXTENSION + AST_MAX_CONTEXT + 32];

	snprintf(args, sizeof(args), "%s,%s,%u",
			context_name, beep_exten, interval);

	if (hook_read(chan, NULL, args, beep_id, len)) {
		ast_log(LOG_WARNING, "Failed to enable periodic beep.\n");
		return -1;
	}

	return 0;
}

int AST_OPTIONAL_API_NAME(ast_beep_stop)(struct ast_channel *chan, const char *beep_id)
{
	return hook_write(chan, NULL, (char *) beep_id, "off");
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS, "Periodic dialplan hooks.",
		.load = load_module,
		.unload = unload_module,
		);
