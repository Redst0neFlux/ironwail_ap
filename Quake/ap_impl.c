//#include "quakedef.h"
// [ap] includes
#include "ap_impl.h"
#include <APCc.h>
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ap_qengine.h>
#include <rapidhash.h>

// Func Defs // AP Impl Funcs
void AP_Initialize (json_t* game_config, ap_connection_settings_t connection);

json_error_t jerror;
json_t* ap_config = NULL;
json_t* ap_connect_info = NULL;
json_t* ap_game_config = NULL;

// ap debug vars
int ap_debug_dive = 0;
int ap_debug_jump = 0;
int ap_debug_run = 0;
int ap_debug_button = 0;
int ap_debug_door = 0;
int ap_debug_grenadesaver = 0;
int ap_debug_rocketsaver = 0;
int ap_debug_rocketjump = 0;
int ap_debug_grenadejump = 0;
int ap_debug_shootswitch = 0;

// quake sync vars
int ap_inventory_flags = 0;
int ap_give_ammo = 0;
int ap_give_ammo_arr[AMMO_MAX]; // 0 shells, 1 spikes, 2 rockets, 3 batteries
int ap_max_ammo_arr[AMMO_MAX]; // 0 shells, 1 spikes, 2 rockets, 3 batteries
int ap_max_ammo_default_arr[AMMO_MAX] = { 25, 30, 5, 15 }; // 0 shells, 1 spikes, 2 rockets, 3 batteries

int ap_give_inv = 0;
int ap_inv_max_arr[INV_MAX]; // 0 quad, 1 invuln, 2 bio, 3 invis, 4 backpack, 5 medkit, 6 armor
int ap_inv_arr[INV_MAX]; // 0 quad, 1 invuln, 2 bio, 3 invis, 4 backpack, 5 medkit, 6 armor

int ap_active_traps[TRAPS_MAX]; // 0 lowhealth, 1 death, 2 mouse, 3 sound, 4 jump

int ap_skill = 0;

int ap_shell_recharge = 0;

int ap_shub_unlocked = 0;
int ap_shub_defeated = 0;

int ap_fresh_map = 0;

int ap_scoreboard = 0;

GArray* scout_reqs;
GHashTableIter iter;

ap_init_state_t ap_global_state = AP_UNINIT;
ap_connection_settings_t ap_connection_settings = { AP_DISABLED, "", 0, "", "", "" };

typedef struct {
	// std::map<ap_net_id_t, uint16_t> persistent;
	GHashTable* persistent;  // Counts of all items received. Do not modify, this is the progression state!
	// std::map<ap_net_id_t, uint16_t> progressive;
	GHashTable* progressive;  // Counts for each progressive item applied. Can be safely cleared when reapplying all items to keep track again
	// std::vector<std::pair<ap_net_id_t, bool>> ap_item_queue;
	GQueue* ap_item_queue;  // Queue of items to be provided to the player whenever he's in-game 
	json_t* dynamic_player;  // Game specific dynamic state. This should be conserved, but contains no progression relevant information
	int need_sync;  // Flag specifying relevant data was changed. If set, will be synced to the AP Server on next opportunity
} ap_state_t;

extern ap_state_t* ap_game_state = NULL;

GQueue* ap_message_queue = NULL;

// rapidhash
uint64_t rapidhash_seed = AP_QUAKE_ID_PREFIX;

// Constructors
ap_state_t* ap_state_new () {
	ap_state_t* state = (ap_state_t*)malloc (sizeof (ap_state_t));
	if (!state) return NULL;

	state->persistent = g_hash_table_new (g_int64_hash, g_int64_equal); //std::map<ap_net_id_t, uint16_t>
	state->progressive = g_hash_table_new (g_int64_hash, g_int64_equal); //std::map<ap_net_id_t, uint16_t>
	state->ap_item_queue = g_queue_new ();
	state->dynamic_player = json_object ();
	state->need_sync = 0;

	return state;
}

item_count_struct* item_count_struct_new (uint64_t net_id, int16_t count) {
	item_count_struct* item_count = malloc (sizeof (item_count_struct));

	if (item_count != NULL) {
		item_count->net_id = net_id;
		item_count->count = count;
	}
	else {
		ap_error ("Memory allocation failed\n");
		exit (0);
	}
	return item_count;
}

uint64t_bool_struct* uint64t_bool_struct_new (ap_net_id_t item_id, bool notify) {
	uint64t_bool_struct* s = malloc (sizeof (uint64t_bool_struct));
	if (s == NULL) {
		return NULL;
	}
	s->item_id = item_id;
	s->notify = notify;
	return s;
}

// Destructors
void ap_state_free (ap_state_t* state) {
	if (!state) return;

	if (state->persistent) g_hash_table_destroy (state->persistent);
	if (state->progressive) g_hash_table_destroy (state->progressive);
	if (state->ap_item_queue) g_queue_free (state->ap_item_queue);
	if (state->dynamic_player) json_decref (state->dynamic_player);

	free (state);
}

void item_count_struct_free (item_count_struct* item_count) {
	if (item_count != NULL) {
		free (item_count);
	}
}

void uint64t_bool_struct_free (uint64t_bool_struct* s) {
	if (s != NULL) {
		free (s);
	}
}


// AP Init
ap_location_state_t ap_locations[AP_MAX_LOCATION];
GArray* scouted_items;
GHashTable* ap_goals = NULL;
GHashTable* ap_item_info = NULL;
GHashTable* ap_game_settings = NULL;
GHashTable* ap_used_level_unlocks = NULL;
GHashTable* ap_used_levels = NULL;
GHashTable* ap_unlocked_levels = NULL;
GHashTable* ap_ability_unlocks = NULL;
GHashTable* ap_automap_unlocks = NULL;
GHashTable* ap_keys_per_level = NULL; // Status represented by keyflags

int ap_received_scout_info = 0;

uint64_t always_spawn_edicts_array[1] = { 3047032649193967426 }; // "e1m7 Sigil (22)"

GHashTable* ability_unlocks;
static GString* remote_id_checksum;

// AP static states
static bool reached_goal = 0;

// AP Win/Con Funcs
void ap_printf (const char* format, ...)
{
	HANDLE hConsole = GetStdHandle (STD_OUTPUT_HANDLE);
	if (hConsole == INVALID_HANDLE_VALUE) {
		fprintf (stderr, "Error getting standard handle.\n");
		return;
	}

	va_list args;
	va_start (args, format);
	int bufferSize = vsnprintf (NULL, 0, format, args) + 1;
	char* buffer = (char*)malloc (bufferSize);

	if (buffer == NULL) {
		fprintf (stderr, "Memory allocation failed.\n");
		return;
	}
	
	vsnprintf (buffer, bufferSize, format, args);
	va_end (args);
	WriteConsoleA (hConsole, buffer, (DWORD)strlen (buffer), NULL, NULL);
	free (buffer);
	return;
}

void ap_printfd (const char* format, ...)
{
	if (!AP_DEBUG) return;
	HANDLE hConsole = GetStdHandle (STD_OUTPUT_HANDLE);
	if (hConsole == INVALID_HANDLE_VALUE) {
		fprintf (stderr, "Error getting standard handle.\n");
		return;
	}

	va_list args;
	va_start (args, format);
	int bufferSize = vsnprintf (NULL, 0, format, args) + 1;
	char* buffer = (char*)malloc (bufferSize);

	if (buffer == NULL) {
		fprintf (stderr, "Memory allocation failed.\n");
		return;
	}

	vsnprintf (buffer, bufferSize, format, args);
	va_end (args);
	WriteConsoleA (hConsole, buffer, (DWORD)strlen (buffer), NULL, NULL);
	free (buffer);
	return;
}

void ap_error (const char* errorMsg, ...)
{
	wchar_t w_msg[1024];
	char buffer[1024];
	va_list args;
	va_start (args, errorMsg);
	vsnprintf (buffer, sizeof (buffer), errorMsg, args);
	va_end (args);
	MultiByteToWideChar (CP_UTF8, 0, buffer, -1, w_msg, sizeof (w_msg) / sizeof (w_msg[0]));
	MessageBoxW (NULL, w_msg, L"AP Error", MB_OK | MB_SETFOREGROUND | MB_ICONSTOP);
}

// Local AP Vars
uint8_t ap_game_id = 0;
char* current_map;

// JSON Funcs
void json_print_sys (const char* key, json_t* j)
{
	char* jdump = json_dumps (j, JSON_DECODE_ANY);
	ap_printf ("%s:\n%s\n", key, jdump);
	free (jdump);
}

// AP Funcs
// 

void ap_on_map_load(char* mapname) {
	current_map = mapname;
}

static ap_location_t safe_location_id (json_t* loc_id)
{
	if (json_is_integer (loc_id))
		return AP_SHORT_LOCATION (json_integer_value(loc_id));
	return 0;
}

static void set_goals (json_t* jobj)
{
	g_hash_table_remove_all (ap_goals);

	const char* k;
	json_t* v;
	json_object_foreach (jobj, k, v)
	{
		ap_net_id_t goal_id = AP_NET_ID (json_integer_value (json_object_get(v, "id")));
		uint16_t goal_count = (uint16_t)json_integer_value (json_object_get (v, "count"));
		item_count_struct* goal_data = item_count_struct_new (goal_id, goal_count);
		GString* gs_key = g_string_new (k);
		g_hash_table_insert (ap_goals, gs_key, goal_data);
	}
}

extern bool AP_IsLevelUsed (ap_net_id_t unlock_key)
{
	ap_net_id_t key = AP_NET_ID (unlock_key);
	uint8_t* status = g_hash_table_lookup (ap_used_level_unlocks, &key);
	if (status && status > 0) {
		return 1;
	}
	return 0;
}

GHashTable* ap_level_data = NULL;

static void ap_parse_levels ()
{
	if (!ap_level_data) ap_level_data = g_hash_table_new (g_string_hash, g_string_equal);
	else g_hash_table_remove_all (ap_level_data);

	const char* k_ep;
	json_t* v_ep;
	json_t* jobj = json_object_get (ap_game_config, "episodes");
	json_object_foreach (jobj, k_ep, v_ep)
	{
		const char* k_lev;
		json_t* v_lev;
		json_t* jobj_lev = json_object_get (v_ep, "levels");
		json_object_foreach (jobj_lev, k_lev, v_lev)
		{
			ap_net_id_t unlock = json_integer_value (json_object_get (v_lev, "unlock"));
			const char* mapfile = json_string_value (json_object_get (v_lev, "mapfile"));
			GString* key = g_string_new (mapfile); 
			if (AP_IsLevelUsed (unlock)) {
				g_hash_table_insert (ap_level_data, key, json_deep_copy (v_lev));
				int active = 1;
				g_hash_table_insert (ap_used_levels, key, &active);
			}
		}
	}
}

static int char_to_uint64 (const char* str, uint64_t* value)
{
	char* endptr;

	uint64_t result = strtoull (str, &endptr, 10); // Base 10 conversion

	if (*endptr != '\0') return 0;

	if (value != NULL) {
		*value = result;
	}

	return 1;
}

static char* uint64_to_char (uint64_t item_id)
{
	char* str = (char*)malloc (20 + 1); //20 derived from max uint64 number
	if (str == NULL) {
		ap_error ("Memory allocation failed!\n");
		return NULL;
	}
	sprintf (str, "%zu", item_id);
	return str;
}

ap_location_t edict_to_ap_locid (uint64_t loc_hash, char* loc_type)
{
	json_t* id_data = NULL;
	json_t* level_data = level_data = json_object_get (json_object_get (json_object_get (ap_game_config, "locations"), current_map), loc_type);
	const char* k;
	json_t* v;
	json_object_foreach (level_data, k, v)
	{
		json_t* uuid_data = json_object_get (v, "uuid");
		uint64_t hash = 0;
		if (char_to_uint64 (json_string_value (uuid_data), &hash)) {
			if (hash == loc_hash) {
				id_data = json_object_get (v, "id");
				return safe_location_id (id_data);
			}
		}
	}
	return AP_INVALID_LOCATION;
}

/*
  Returns if a spawning edict needs to be patched or deleted
  Replace Progression = 2
  Replace = 1
  Delete = 0
*/
extern int ap_replace_edict (uint64_t loc_hash, char* loc_type)
{
	char* map = current_map;
	json_t* item_locations = json_object_get (json_object_get (json_object_get (ap_game_config, "locations"), map), loc_type);
	
	ap_location_t item_location  = edict_to_ap_locid (loc_hash, loc_type);

	if (!AP_VALID_LOCATION (item_location)) {	
		return 0;
	}

	const char* k_item;
	json_t* v_item;
	json_object_foreach (item_locations, k_item, v_item) {
		uint64_t item_id = json_integer_value (json_object_get (v_item, "id"));
		if (item_id == item_location) {
			if (AP_LOCATION_PROGRESSION (item_location)) 
				return 2;
			return 1;
		}
	}
	return 0;
}

/*
  Return if an edict is already collected
  Delete = 1
  Do Nothing = 0
*/
extern int ap_free_collected_edicts (uint64_t loc_hash, char* loc_type)
{
	ap_location_t item_location = edict_to_ap_locid (loc_hash, loc_type);
	// Check allow list entries
	int array_size = sizeof (always_spawn_edicts_array) / sizeof (always_spawn_edicts_array[0]);
	for (int i = 0; i < array_size; i++) {
		if (loc_hash == always_spawn_edicts_array[i]) {
			return 0;
		}
	}
	if (AP_LOCATION_CHECKED (item_location)) return 1;

	return 0;
}

void ap_remaining_items (uint16_t* collected, uint16_t* total, char* mapname)
{
	json_t* level_data = level_data = json_object_get (json_object_get (json_object_get (ap_game_config, "locations"), mapname), "items");
	const char* k_itemkey;
	json_t* v_itemdata;
	json_object_foreach (level_data, k_itemkey, v_itemdata)
	{
		ap_location_t pickup_loc = (ap_location_t)json_integer_value (json_object_get (v_itemdata, "id"));
		if (pickup_loc > 0 && AP_LOCATION_CHECK_MASK (pickup_loc, (AP_LOC_PICKUP | AP_LOC_USED)))
		{
			(*total)++;
			if (AP_LOCATION_CHECKED (pickup_loc)) 
				(*collected)++;
		}
	}
}

void ap_remaining_secrets (uint16_t* collected, uint16_t* total, char* mapname)
{
	json_t* level_data = level_data = json_object_get (json_object_get (json_object_get (ap_game_config, "locations"), mapname), "secrets");
	const char* k_itemkey;
	json_t* v_itemdata;
	json_object_foreach (level_data, k_itemkey, v_itemdata)
	{
		ap_location_t secret_loc = (ap_location_t)json_integer_value (json_object_get (v_itemdata, "id"));
		if (secret_loc > 0 && AP_LOCATION_CHECK_MASK (secret_loc, (AP_LOC_SECRET | AP_LOC_USED)))
		{
			(*total)++;
			if (AP_LOCATION_CHECKED (secret_loc))
				(*collected)++;
		}
	}
}

void ap_remaining_exits (uint16_t* collected, uint16_t* total, char* mapname)
{
	json_t* level_data = level_data = json_object_get (json_object_get (json_object_get (ap_game_config, "locations"), mapname), "exits");
	const char* k_itemkey;
	json_t* v_itemdata;
	json_object_foreach (level_data, k_itemkey, v_itemdata)
	{
		ap_location_t secret_loc = (ap_location_t)json_integer_value (json_object_get (v_itemdata, "id"));
		if (secret_loc > 0 && AP_LOCATION_CHECK_MASK (secret_loc, (AP_LOC_SECRET | AP_LOC_USED)))
		{
			(*total)++;
			if (AP_LOCATION_CHECKED (secret_loc))
				(*collected)++;
		}
	}
}

// Helpers
uint64_t generate_hash (float f1, float f2, float f3, const char* itemname) {

	int32_t i1 = (int32_t)floorf (f1);
	int32_t i2 = (int32_t)floorf (f2);
	int32_t i3 = (int32_t)floorf (f3);

	GString* inp_val = g_string_new (NULL);
	g_string_printf (inp_val, "%d_%d_%d_%s", i1, i2, i3, itemname);
	
	uint64_t hash = rapidhash_withSeed (inp_val->str, inp_val->len, rapidhash_seed);

	return hash;
}

const char* ap_jtype_to_string (json_t* j)
{
	json_type jt = json_typeof (j);
	switch (jt) {
	case JSON_OBJECT:
		return "JSON_OBJECT";
	case JSON_ARRAY:
		return "JSON_ARRAY";
	case JSON_STRING:
		return "JSON_STRING";
	case JSON_INTEGER:
		return "JSON_INTEGER";
	case JSON_TRUE:
		return "JSON_BOOL";
	case JSON_FALSE:
		return "JSON_BOOL";
	case JSON_REAL:
		return "JSON_REAL";
	case JSON_NULL:
		return "JSON_NULL";
	default:
		return "Unknown JSON type";
	}
}

static void* ght_lookup_str (GHashTable* ght, char* key)
{
	GString* lookup = g_string_new (key);
	if (!g_hash_table_lookup (ght, lookup)) return NULL;
	void* result = g_hash_table_lookup (ght, lookup);
	g_string_free (lookup, TRUE);
	return result;
}

int touched_edicts_count = 0;
char* touched_edicts_list[1024] = { NULL };

extern void add_touched_edict (uint64_t loc_hash, char* loc_type)
{
	char* out_str = edict_get_loc_name (loc_hash, loc_type);

	if (touched_edicts_count >= 1024) {
		return;
	}

	for (int i = 0; i < touched_edicts_count; i++) {
		if (strcmp (touched_edicts_list[i], out_str) == 0) {
			return;
		}
	}

	touched_edicts_list[touched_edicts_count] = strdup (out_str);
	if (touched_edicts_list[touched_edicts_count] != NULL) {
		touched_edicts_count++;
	}
	for (int i = 0; i < touched_edicts_count; i++) {
		ap_printfd ("\"%s\",\n", touched_edicts_list[i]);
	}
	ap_printfd ("\n");
}

extern void clear_touched_edict_list ()
{
	for (int i = 0; i < touched_edicts_count; i++) {
		free (touched_edicts_list[i]);
		touched_edicts_list[i] = NULL;
	}
	touched_edicts_count = 0;
	ap_printf ("Cleared edict list.\n");
}

// AP Callback Funcs
void AP_ItemReceived (uint64_t item_id, int slot, bool notify)
{
	if (!g_hash_table_lookup (ap_item_info, &item_id)) 
		return; // Don't know anything about this type of item, ignore it

	// Push item and notification flag to queue
	uint64t_bool_struct* item_data = uint64t_bool_struct_new (item_id, notify);
	if (item_data == NULL) {
		return;
	}
	g_queue_push_tail (ap_game_state->ap_item_queue, item_data);
	ap_printfd ("Received: %s\n", AP_GetItemName (item_id));
}

void AP_ClearAllItems ()
{
	g_queue_clear (ap_game_state->ap_item_queue);
	g_hash_table_remove_all (ap_game_state->persistent);
}

void AP_ExtLocationCheck (uint64_t location_id)
{
	ap_location_t loc = AP_SHORT_LOCATION (location_id);
	// Check if the location is even a valid id for current game
	if (!AP_VALID_LOCATION_ID (loc))
		return;
	// Check if we already have this location confirmed as checked.
	if (AP_LOCATION_CHECKED (loc))
		return;
	ap_printfd ("Marking location %i (NET_ID %zu) as checked\n", loc, location_id);
	ap_locations[loc].state |= (AP_LOC_CHECKED);
}

int AP_CheckLocation (uint64_t loc_hash, char* loc_type)
{
	ap_location_t loc = edict_to_ap_locid (loc_hash, loc_type);
	// Check if the location is even a valid id for current game
	if (!AP_VALID_LOCATION (loc))
		return 0;
	// Check if we already have this location confirmed as checked.
	if (AP_LOCATION_CHECKED (loc))
		return 0;
	// Forward check to AP server
	ap_net_id_t net_loc = AP_NET_ID (loc);
	AP_SendItem (AP_NET_ID (net_loc));
	ap_locations[loc].state |= AP_LOC_CHECKED;
	// And note the location check in the console
	ap_printfd ("New Check: %s\n", AP_GetLocationName (net_loc));
	return 1;
}

// std::vector<AP_NetworkItem> scouted_items
void AP_LocationInfo (GArray* scouted_items_in)
{
	// ToDo can't distinguish this from a received hint?
	// How do we decide when we can legally store the item content
	// as something the player should know about?
	for (guint i = 0; i < scouted_items_in->len; i++)
	{
		struct AP_NetworkItem* item = g_array_index (scouted_items_in, struct AP_NetworkItem*, i);
		ap_location_t loc = AP_SHORT_LOCATION (item->location);
		if (!AP_VALID_LOCATION_ID (loc))
			continue;
		ap_locations[loc].item = item->item;
		// Set state based on active flags
		if (item->flags & 0b001)
			ap_locations[loc].state |= AP_LOC_PROGRESSION;
		if (item->flags & 0b010)
			ap_locations[loc].state |= AP_LOC_IMPORTANT;
		if (item->flags & 0b100)
			ap_locations[loc].state |= AP_LOC_TRAP;
	}
	ap_received_scout_info = 1;
}

uint16_t AP_ItemCount (ap_net_id_t id)
{
	uint16_t* count = g_hash_table_lookup (ap_game_state->persistent, &id);
	return count ? *count : 0;
}

bool AP_HasItem (ap_net_id_t id)
{
	return AP_ItemCount (id) > 0;
}

/* Increments the progressive count for an item in the state and returns the current total count */
uint16_t AP_ProgressiveItem (ap_net_id_t id)
{
	uint16_t* count = g_hash_table_lookup (ap_game_state->progressive, &id);
	if (count) {
		(*count)++;
	}
	else {
		count = malloc (sizeof (uint16_t));
		if (count) *count = 1;
		uint64_t* key = malloc (sizeof (uint64_t));
		if (key != NULL) *key = id;
		g_hash_table_insert (ap_game_state->progressive, key, count);
	}
	if (count) return *count;
	else return 0;
}

// TODO: Test sync/resync
void AP_SyncProgress (void)
{
	if (!ap_game_state->need_sync) return;

	// Wait until initialization is done before writing back, only then things are consistent
	if (!ap_global_state == AP_INITIALIZED) return;

	json_t* save_data = json_object();
	json_t* player_obj = ap_game_state->dynamic_player;

	if (player_obj)	{
		json_object_set (save_data, "player", json_deep_copy(player_obj));
	}
	else {
		// if there is no player save data, simply return
		return;
	}

	char* serialized = json_dumps(save_data, JSON_COMPACT);

	const char* prefix = AP_GetPrivateServerDataPrefix ()->str;
	const char* suffix = "_save_data";
	size_t total_length = strlen (prefix) + strlen (suffix) + 1;
	char* key = (char*)malloc (total_length);
	if (key == NULL) {
		ap_error ("Memory allocation failed\n");
		return;
	}
	strcpy (key, prefix);
	strcat (key, suffix);

	struct AP_DataStorageOperation* replace_op = AP_DataStorageOperation_new ("replace", serialized);
	GArray* operations_array = g_array_new (true, true, sizeof (struct AP_DataStorageOperation*));
	g_array_append_val (operations_array, replace_op);
	char* def_val = "";
	struct AP_SetServerDataRequest* requ = AP_SetServerDataRequest_new (Pending, key, operations_array, def_val, Raw, false);
	AP_SetServerData (requ);

	ap_game_state->need_sync = false;
}



VictoryStats AP_VictoryStats (char* victory_name)
{
	item_count_struct* value;

	value = ght_lookup_str (ap_goals, victory_name);
	if (value == NULL) {
		VictoryStats empty_stats = { 0, 0 };
		return empty_stats;
	}

	VictoryStats stats;
	stats.item_count = AP_ItemCount (value->net_id);
	stats.total = value->count;

	return stats;
}

bool AP_CheckVictory (void)
{
	if (reached_goal) return false; // Already reached victory state once
	bool all_reached = true;
	
	GString* key;
	item_count_struct* value;

	g_hash_table_iter_init (&iter, ap_goals);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		if (AP_ItemCount (value->net_id) < value->count) {
			all_reached = false;
			break;
		}
	}

	if (all_reached)
	{
		// Unlock end map
		// Send victory state to AP Server
		reached_goal = true;
		json_t* player_obj = json_object_get (ap_game_state->dynamic_player, "player");
		//TODO: Is this the correct json structure?
		json_object_set (player_obj, "victory", json_integer(1));
		ap_game_state->need_sync = true;
		AP_StoryComplete ();
		g_queue_push_tail (ap_message_queue, _strdup ("Goal reached!\n"));
	}
	return reached_goal;
}

void AP_QueueMessage (char* msg)
{
	ap_printf (msg);
	g_queue_push_tail (ap_message_queue, _strdup(msg));
}

// TODO: Work on colored console message output
void AP_ProcessMessages ()
{
	while (AP_IsMessagePending ())
	{
		struct AP_Message* msg = AP_GetLatestMessage ();
		GString* print_msg = g_string_new (NULL);

		switch (msg->type)
		{
			case ItemSend:
			{
				struct AP_ItemSendMessage* out_msg = AP_GetLatestMessage ();
				g_string_printf (print_msg, "%s was sent to %s\n", out_msg->item, out_msg->recvPlayer);
				break;
			}
			case ItemRecv:
			{
				struct AP_ItemRecvMessage* out_msg = AP_GetLatestMessage ();
				g_string_printf (print_msg, "Received %s from %s\n", out_msg->item, out_msg->sendPlayer);
				break;
			}
			case Hint:
			{
				struct AP_HintMessage* out_msg = AP_GetLatestMessage ();
				char* status = out_msg->checked ? "(Checked)" : "(Unchecked)";
				g_string_printf (print_msg, "%s from %s for %s is at %s %s\n", out_msg->item, out_msg->sendPlayer, out_msg->recvPlayer, out_msg->location, status);
				break;
			}
			default:
			{
				g_string_printf (print_msg, msg->text ) ;
				g_string_append (print_msg, "\n");
				break;
			}
		}
		g_queue_push_tail (ap_message_queue, print_msg->str);

		AP_ClearLatestMessage ();
	}
}

// Safe check if an item is scoped to a level
static bool item_for_level (json_t* info, char* mapfile)
{
	if (!current_map) return 0;
	// Check if mapfile name is in location name
	const char* loc_name = json_string_value(json_object_get (info, "name"));
	return (strstr (loc_name, mapfile) != NULL);
}

static bool item_for_current_level (json_t* info)
{
	return item_for_level (info, current_map);
}

void ap_set_default_inv () {
	// Clear inventory info
	for (uint8_t i = 0; i < INV_MAX; i++)
	{
		ap_inv_arr[i] = 0;
		ap_inv_max_arr[i] = 0;
	}
	// Clear weapon info
	for (uint8_t i = 0; i < AMMO_MAX; i++)
	{
		ap_give_ammo_arr[i] = 0;
		ap_max_ammo_arr[i] = ap_max_ammo_default_arr[i];
		if (AP_DEBUG_SPAWN) {
			ap_give_ammo_arr[i] = 100;
			ap_max_ammo_arr[i] = 100;
		}
	}
	ap_give_ammo = 0;
	ap_give_inv = 0;
	ap_inventory_flags = 0;

	//TODO: Do ability unlocks need to be cleared?
}

GHashTable* ap_debug_edict_lut = NULL;

void ap_debug_init ()
{
	if(!ap_debug_edict_lut) ap_debug_edict_lut = g_hash_table_new (g_int64_hash, g_int64_equal);
	ap_set_default_inv ();
}

void ap_debug_add_edict_to_lut (uint64_t loc_hash, char* loc_name)
{
	char* value = _strdup (loc_name);
	uint64_t* key = malloc (sizeof (uint64_t));
	if (key) *key = loc_hash;
	g_hash_table_insert (ap_debug_edict_lut, key, value);
}

char* edict_get_loc_name (uint64_t loc_hash, char* loc_type) 
{
	if (AP_DEBUG_SPAWN) {
		char* ret_val = g_hash_table_lookup (ap_debug_edict_lut, &loc_hash);
		return ret_val;
	}
	ap_location_t loc = edict_to_ap_locid (loc_hash, loc_type);
	// Check if the location is even a valid id for current game
	if (!AP_VALID_LOCATION (loc))
		return "";
	ap_net_id_t net_loc = AP_NET_ID (loc);
	return AP_GetLocationName (net_loc);
}

/*
  Apply whatever item we just got to our current game state

  Upgrade only provides just the unlock, but no ammo/capacity. This is used
  when loading savegames.
*/
static void ap_get_item (ap_net_id_t item_id, bool silent, bool is_new)
{
	json_t* item_info = g_hash_table_lookup (ap_item_info, &item_id);
	if (!item_info) return;
	// Check if we have a dynamic override for the item in our seed slot data
	GString* gs_key = g_string_new ("dynamic");
	json_t* dynamic_info = g_hash_table_lookup (ap_game_settings, gs_key);

	char* id_string = uint64_to_char (item_id);
	if (dynamic_info) {
		if (json_object_get (dynamic_info, id_string) != NULL)
			item_info = json_object_get (dynamic_info, id_string);
	}
	//bool notify = json_boolean_value (json_object_get(item_info, "silent"));
	
	// Store counts for stateful items
	if (is_new && json_boolean_value (json_object_get (item_info, "persistent")))
	{
		uint16_t* count = malloc (sizeof (uint16_t));
		*count = 0;
		ap_net_id_t* key = malloc (sizeof (ap_net_id_t));
		if (key) *key = item_id;
		if (AP_HasItem (item_id)) {
			count = (uint16_t*)g_hash_table_lookup (ap_game_state->persistent, &item_id);
		}
		*count += 1;

		if (json_integer_value (json_object_get (item_info, "unique")))
			*count = 1;
		g_hash_table_insert (ap_game_state->persistent, key, count);
	}

	const char* item_type = json_string_value(json_object_get (item_info, "type"));

	if (!strcmp (item_type, "progressive")) {
		// Add to our progressive counter and check how many we have now
		uint16_t prog_count = AP_ProgressiveItem (item_id);
		// And apply whatever item we have next in the queue
		json_t* items_array = json_object_get (item_info, "items");
		if (items_array) {
			size_t arr_size = json_array_size (items_array);
			size_t idx = (arr_size < prog_count ? arr_size : prog_count) - 1;
			ap_net_id_t next_item = AP_NET_ID (json_integer_value (json_array_get(items_array, idx)));
			ap_get_item (next_item, silent, false);
		}
	}
	else if (!strcmp (item_type, "key") && item_for_current_level (item_info)) {
		// apply key flag to inventory flags
		uint64_t flags = json_integer_value (json_object_get (item_info, "flags"));
		ap_inventory_flags |= flags;
	}
	else if (!strcmp (item_type, "key")) {
		// add key to lookup for map menu
		GString* gs_key = g_string_new (json_string_value (json_object_get (item_info, "mapfile")));
		uint64_t flags = json_integer_value (json_object_get (item_info, "flags"));
		// check if key is present
		uint64_t* val = g_hash_table_lookup (ap_keys_per_level, gs_key);
		if (val) 
			*val |= flags;
		else {
			uint64_t* new_val =  (uint64_t*)malloc (sizeof (uint64_t));
			*new_val = 0 | flags;
			g_hash_table_insert (ap_keys_per_level, gs_key, new_val);
		}
	}
	else if (!strcmp (item_type, "map")) {
		GString* gs_key = g_string_new (json_string_value (json_object_get (item_info, "mapfile")));
		uint8_t unlocked = 1;
		g_hash_table_insert (ap_unlocked_levels, gs_key, &unlocked);
	}
	else if (!strcmp (item_type, "automap")) {
		GString* gs_key = g_string_new (json_string_value (json_object_get (item_info, "mapfile")));
		uint8_t unlocked = 1;
		g_hash_table_insert (ap_automap_unlocks, gs_key, &unlocked);
	}
	else if (!strcmp (item_type, "weapon")) {
		uint64_t flags = json_integer_value (json_object_get (item_info, "flags"));
		ap_inventory_flags |= flags;

		// grab ammonum and fill give_arr
		int ammonum = (int)json_integer_value (json_object_get (item_info, "ammonum"));
		int ammo = (int)json_integer_value (json_object_get (item_info, "ammo"));
		ap_give_ammo_arr[ammonum] += ammo;
		ap_give_ammo = 1;
	}
	else if (!strcmp (item_type, "ammo") || !strcmp (item_type, "maxammo")) {
		int ammonum = (int)json_integer_value (json_object_get (item_info, "ammonum"));
		int ammo = (int)json_integer_value (json_object_get (item_info, "ammo"));
		ap_give_ammo_arr[ammonum] += ammo;
		// check if the pickup also increases ammo capacity
		json_t* capacity_obj = json_object_get (item_info, "capacity");
		if (capacity_obj) {
			int capacity = (int)json_integer_value (capacity_obj);
			ap_max_ammo_arr[ammonum] += capacity;
		}
		ap_give_ammo = 1;
	}
	else if (!strcmp (item_type, "inventory")) {
		int invnum = (int)json_integer_value (json_object_get (item_info, "invnum"));
		double capacity = (double)json_integer_value (json_object_get (item_info, "capacity"));
		double factor = 1;
		// apply mult factor for health and armor
		json_t* factor_obj = json_object_get (item_info, "factor");
		if (factor_obj)
			factor = json_real_value (factor_obj);
		capacity *= factor;
		// round down capacity and apply
		int rounded_capacity = (int)floor (capacity);
		ap_inv_max_arr[invnum] += rounded_capacity;
		ap_inv_arr[invnum] += rounded_capacity;
		ap_give_inv = 1;
	}
	else if (!strcmp (item_type, "invcapacity")) {
		int invnum = (int)json_integer_value (json_object_get (item_info, "invnum"));
		double capacity = (double)json_integer_value (json_object_get (item_info, "capacity"));
		double factor = 1;
		// apply mult factor for health and armor
		json_t* factor_obj = json_object_get (item_info, "factor");
		if (factor_obj)
			factor = json_real_value (factor_obj);
		capacity *= factor;
		// round down capacity and apply
		int rounded_capacity = (int)floor (capacity);
		ap_inv_max_arr[invnum] += rounded_capacity;
		ap_inv_arr[invnum] += rounded_capacity;
		ap_give_inv = 1;
	}
	else if (!strcmp (item_type, "ability")) {
		GString* gs_key = g_string_new(json_string_value(json_object_get(item_info, "enables")));
		uint8_t unlocked = 1;
		g_hash_table_insert (ap_ability_unlocks, gs_key, &unlocked);
	}
	else if (!strcmp (item_type, "trap") && !silent) {
		json_t* traps_obj = json_object_get (ap_game_state->dynamic_player, "traps");
		json_t* trap_state = json_object_get (traps_obj, uint64_to_char(item_id));

		if (!trap_state)
		{
			// New trap type, initialize
			trap_state = json_object ();
			json_object_set (trap_state, "id", json_integer(item_id));
			json_object_set (trap_state, "count", json_integer (1));
			json_object_set (trap_state, "remaining", json_integer (0));
			json_object_set (trap_state, "grace", json_integer (0));
			
			json_object_set (traps_obj, uint64_to_char (item_id), trap_state);
		}
		else
		{
			// Just increment the queue trap count
			uint64_t count = json_integer_value (json_object_get (trap_state, "count"));
			json_object_set (trap_state, "count", json_integer (count+1));
		}
	}
}

void ap_sync_inventory () {
	g_hash_table_remove_all(ap_game_state->progressive);

	ap_set_default_inv ();

	// Apply state for all persistent items we have unlocked
	gpointer id;
	gpointer count;
	g_hash_table_iter_init (&iter, ap_game_state->persistent);
	while (g_hash_table_iter_next (&iter, &id, &count)) {
		uint64_t* id_ptr = (uint64_t*)id;
		uint16_t* count_ptr = (uint16_t*)count;
		for (int i = 0; i < *count_ptr; i++) {
			ap_get_item (*id_ptr, 1, 0);
		}
	}
}

extern char* ap_get_latest_message () {
	if (!g_queue_is_empty (ap_message_queue)) {
		return g_queue_pop_head (ap_message_queue);
	}
	return NULL;
}

extern int ap_message_pending () {
	if (ap_message_queue) return ap_message_queue->length > 0;
	return 0;
}

void ap_handle_trap (json_t* trap_info, bool triggered) {
	
	if (triggered) {
		const char* trap_name = json_string_value (json_object_get (trap_info, "name"));
		GString* msg = g_string_new (NULL);
		g_string_append (msg, trap_name);
		g_string_append (msg, " triggered!\n");
		AP_QueueMessage (msg->str);
	}

	// Set trap states
	const char* trap_type = json_string_value (json_object_get (trap_info, "trap"));
	if (!strcmp (trap_type, "lowhealth")) ap_active_traps[0] = 1;
	if (!strcmp (trap_type, "death")) ap_active_traps[1] = 1;
	if (!strcmp (trap_type, "mouse")) ap_active_traps[2] = 1;
	if (!strcmp (trap_type, "sound")) ap_active_traps[3] = 1;
	if (!strcmp (trap_type, "jump")) ap_active_traps[4] = 1;
}

void ap_disable_trap (json_t* trap_info) {
	// Set trap states
	const char* trap_type = json_string_value (json_object_get (trap_info, "trap"));
	if (!strcmp (trap_type, "lowhealth")) ap_active_traps[0] = 0;
	if (!strcmp (trap_type, "death")) ap_active_traps[1] = 0;
	if (!strcmp (trap_type, "mouse")) ap_active_traps[2] = 0;
	if (!strcmp (trap_type, "sound")) ap_active_traps[3] = 0;
	if (!strcmp (trap_type, "jump")) ap_active_traps[4] = 0;
}

// runs every ingame tic

clock_t last_second_60 = 0;
clock_t last_second_1 = 0;

extern void ap_process_ingame_tic (void)
{
	if (ap_global_state != AP_INITIALIZED) return;

	// Check for items in our queue to process
	while (!g_queue_is_empty (ap_game_state->ap_item_queue)) {
		uint64t_bool_struct* queue_item = g_queue_pop_head (ap_game_state->ap_item_queue);
		ap_get_item (queue_item->item_id, !queue_item->notify, true);
	}

	// Check for outstanding or active traps
	json_t* trap_obj = json_object_get (ap_game_state->dynamic_player, "traps");
	const char* k;
	json_t* v;
	json_object_foreach (trap_obj, k, v) {
		// Fetch relevant trap information for this trap type

        json_t* trap_state = v;
		ap_net_id_t trap_id = json_integer_value(json_object_get (trap_state, "id"));
		json_t* trap_info = g_hash_table_lookup (ap_item_info, &trap_id);

		// Check if we have a dynamic override for the trap in our seed slot data
		GString* gs_key = g_string_new ("dynamic");
		json_t* dynamic_info = g_hash_table_lookup (ap_game_settings, gs_key);

		char* id_string = uint64_to_char (trap_id);
		if (dynamic_info) {
			if (json_object_get (dynamic_info, id_string) != NULL)
				trap_info = json_object_get (dynamic_info, id_string);
		}

		
		bool triggered = false;
        bool active = false;
        uint64_t remaining = json_integer_value(json_object_get(trap_state, "remaining"));
        if (remaining > 1)
        {
            remaining--;
			json_object_set (trap_state, "remaining", json_integer(remaining));
            active = true;
        }
        else if (remaining == 1)
        {
            // Disable trap and set a grace period
			json_object_set (trap_state, "remaining", json_integer (0));
			json_object_set (trap_state, "remaining", json_integer (0));
			json_object_set (trap_state, "grace", json_object_get (trap_info, "grace"));
            active = false;
        }
        
        if (!active)
        {
            uint64_t grace = json_integer_value (json_object_get (trap_state, "grace"));
            uint64_t count = json_integer_value (json_object_get (trap_state, "count"));
            if (grace > 0)
            {
                grace--;
                json_object_set(trap_state, "grace", json_integer (grace));
            }
            else if (count > 0)
            {
                // Trigger new trap instance
				json_object_set (trap_state, "remaining", json_object_get (trap_info, "duration"));
                count--;
				json_object_set (trap_state, "count", json_integer (count));
                triggered = true;
                active = true;
            }
        }

        if (active)
        {
			ap_handle_trap (trap_info, triggered);
        }
		else {
			ap_disable_trap (trap_info);
		}
	}

	// Grab current time for time-based recharges

	clock_t current_time = clock ();

	double elapsed_seconds_60 = (double)(current_time - last_second_60) / CLOCKS_PER_SEC;
	double elapsed_seconds_1 = (double)(current_time - last_second_1) / CLOCKS_PER_SEC;

	// TODO: Keep track of individual "last item use" times here for more consistent recharging
	// For now recharge inventory items every 60 seconds
	if (elapsed_seconds_60 >= 60.0) {
		for (int i = 0; i < INV_MAX; i++)
		{
			if (ap_inv_arr[i] < ap_inv_max_arr[i])
				ap_inv_arr[i] += 1;
			ap_give_inv = 1;
		}
		last_second_60 = current_time;
	}

	// Recharge 1 shell per second
	if (ap_shell_recharge && elapsed_seconds_1 >= 1.0) {
		ap_give_ammo = 1;
		// make sure to not overwrite if there are active values present
		if (ap_give_ammo_arr[0] == 0)
			ap_give_ammo_arr[0] = 1;
		last_second_1 = current_time;
	}
	// Handle messages
	AP_ProcessMessages ();
}

// runs in the main loop
extern void ap_process_global_tic (void)
{
	if (ap_global_state != AP_INITIALIZED) return;

	// Check for items in our queue to process
	GQueue* temp_queue = g_queue_new ();
	while (!g_queue_is_empty (ap_game_state->ap_item_queue)) {
		uint64t_bool_struct* queue_item = g_queue_pop_head (ap_game_state->ap_item_queue);
		// Check if the item can be processed in the global loop
		json_t* item_info = g_hash_table_lookup (ap_item_info, &queue_item->item_id);
		if (!item_info) return;

		const char* item_type = json_string_value (json_object_get (item_info, "type"));
		if (!strcmp(item_type, "map") || !strcmp (item_type, "trap") || !strcmp (item_type, "key") || !strcmp (item_type, "goal")){
			ap_get_item (queue_item->item_id, !queue_item->notify, true);
		}
		else {
			g_queue_push_tail (temp_queue, queue_item);
		}
	}

	ap_game_state->ap_item_queue = temp_queue;

	AP_SyncProgress ();

	if (AP_CheckVictory ())
	{
		//TODO: Do something on victory
	}
	AP_ProcessMessages ();
}

extern void ap_set_inventory_to_max (void) {
	for (int i = 0; i < INV_MAX; i++)
	{
		ap_inv_arr[i] = ap_inv_max_arr[i];
	}
}


void AP_LibShutdown (void)
{
	AP_SyncProgress ();
	AP_Shutdown ();
}


// AP Inits

void ap_init_connection ()
{
	ap_config = json_load_file ("ap_config.json", 0, &jerror);
	if (!ap_config) {
		ap_error ("ap_config.json not found.");
		exit (0);
		return;
	}
	// TODO: This should probably run over an ingame menu later
	// For now grab values from ap_connect_info.json
	json_t* ap_connect_info = json_load_file ("ap_connect_info.json", 0, &jerror);
	if (!ap_connect_info) {
		ap_error ("ap_connect_info.json not found.");
		exit (0);
		return;
	}

	ap_connection_settings.mode = AP_SERVER;
	ap_connection_settings.game = json_string_value (json_object_get (ap_config, "game"));
	ap_connection_settings.ip = json_string_value (json_object_get (ap_connect_info, "ip"));
	ap_connection_settings.port = (int)json_integer_value (json_object_get (ap_connect_info, "port"));
	ap_connection_settings.player = json_string_value (json_object_get (ap_connect_info, "player"));
	ap_connection_settings.password = json_string_value (json_object_get (ap_connect_info, "password"));

	AP_Initialize (ap_config, ap_connection_settings);

	if (AP)
	{
		ap_parse_levels ();
	}
}

int ap_can_dive()
{
	if (AP_DEBUG_SPAWN && ap_debug_dive) return 1;
	if (ght_lookup_str (ap_ability_unlocks, "dive")) return 1;
    return 0;
}

int ap_can_jump()
{
	if (AP_DEBUG_SPAWN && ap_debug_jump) return 1;
	if (ght_lookup_str (ap_ability_unlocks, "jump")) return 1;
	return 0;
}

int ap_can_run ()
{
	if (AP_DEBUG_SPAWN && ap_debug_run) return 1;
	if (ght_lookup_str (ap_ability_unlocks, "run")) return 1;
	return 0;
}

int ap_can_button()
{
	if (AP_DEBUG_SPAWN && ap_debug_button) return 1;
	if (ght_lookup_str (ap_ability_unlocks, "button")) return 1;
	return 0;
}

int ap_can_door()
{
	if (AP_DEBUG_SPAWN && ap_debug_door) return 1;
	if (ght_lookup_str (ap_ability_unlocks, "door")) return 1;
	return 0;
}

int ap_can_automap (char* mapname)
{	
	if (AP_DEBUG_SPAWN) return 1;
	if (ght_lookup_str (ap_automap_unlocks, mapname)) return 1;
	return 0;
}

uint64_t* ap_get_key_flags (const char* mapname)
{
	GString* gs_key = g_string_new (mapname);
	// check if key is present
	uint64_t* val = g_hash_table_lookup (ap_keys_per_level, gs_key);
	if (val) return val;
	else return 0;
}

int ap_can_grenadesaver ()
{
	if (AP_DEBUG_SPAWN && ap_debug_grenadesaver) return AP_GRENADESAVER;
	if (ght_lookup_str (ap_ability_unlocks, "grenadedmgremover")) return AP_GRENADESAVER;
	return 0;
}

int ap_can_rocketsaver ()
{
	if (AP_DEBUG_SPAWN && ap_debug_rocketsaver) return AP_ROCKETSAVER;
	if (ght_lookup_str (ap_ability_unlocks, "rocketdmgremover")) return AP_ROCKETSAVER;
	return 0;
}

int ap_can_rocketjump ()
{
	if (AP_DEBUG_SPAWN && ap_debug_rocketjump) return AP_ROCKETJUMP;
	if (ght_lookup_str (ap_ability_unlocks, "rocketjump")) return AP_ROCKETJUMP;
	return 0;
}

int ap_can_grenadejump ()
{
	if (AP_DEBUG_SPAWN && ap_debug_grenadejump) return AP_GRENADEJUMP;
	if (ght_lookup_str (ap_ability_unlocks, "grenadejump")) return AP_GRENADEJUMP;
	return 0;
}

int ap_can_shootswitch ()
{
	if (AP_DEBUG_SPAWN && ap_debug_shootswitch) return AP_SHOOTSWITCH;
	if (ght_lookup_str (ap_ability_unlocks, "shootswitch")) return AP_SHOOTSWITCH;
	return 0;
}

int ap_is_level_used (char* mapname) {
	if (AP_DEBUG_SPAWN) return 1;
	//if (ap_shub_unlocked && !strcmp (mapname, "end")) return 1;
	if (ght_lookup_str (ap_used_levels, mapname)) return 1;
	return 0;
}

int ap_is_level_unlocked (char* mapname) {
	if (AP_DEBUG_SPAWN) return 1;
	//if (ap_shub_unlocked && !strcmp (mapname, "end")) return 1;
	if (ght_lookup_str (ap_unlocked_levels, mapname)) return 1;
	return 0;
}


// Func for setting QuakeC Vars
extern int ap_get_quakec_apflag ()
{
	int flag = 0;
	flag ^= ap_can_rocketsaver();
	flag ^= ap_can_rocketjump();
	flag ^= ap_can_grenadejump();
	flag ^= ap_can_grenadesaver();
	flag ^= ap_can_shootswitch();
	return flag;
}

static void init_item_table (json_t* items)
{
	g_hash_table_remove_all (ap_item_info);
	
	const char* k;
	json_t* v;
	json_object_foreach (items, k, v)
	{
		uint64_t id = 0;
		if (char_to_uint64 (k, &id)) {
			ap_net_id_t* item_id = (ap_net_id_t*)malloc (sizeof (ap_net_id_t));
			if (item_id) {
				*item_id = AP_NET_ID (id);
				json_t* item_data = v;
				g_hash_table_insert (ap_item_info, item_id, json_deep_copy(item_data));
			}
		}
	}
}

static void init_location_table (json_t* locations)
{
	memset (ap_locations, 0, AP_MAX_LOCATION * sizeof (ap_location_state_t));

	// Iterate through the game config data to set the relevant flags for all known locations
	const char* k_level;
	json_t* v_level;
	json_object_foreach (locations, k_level, v_level)
	{
		const char* k_loctype;
		json_t* v_loctype;
		json_object_foreach (v_level, k_loctype, v_loctype)
		{
			if (!strcmp (k_loctype, "items"))
			{
				const char* k_itemkey;
				json_t* v_itemdata;
				json_object_foreach (v_loctype, k_itemkey, v_itemdata)
				{
					uint64_t item_id = json_integer_value(json_object_get (v_itemdata, "id"));
					if (item_id >= 0) ap_locations[AP_SHORT_LOCATION (item_id)].state |= (AP_LOC_PICKUP);
				}
			}
			else if (!strcmp (k_loctype, "secrets"))
			{
				const char* k_secretkey;
				json_t* v_secretdata;
				json_object_foreach (v_loctype, k_secretkey, v_secretdata)
				{
					uint64_t secret_id = json_integer_value (json_object_get (v_secretdata, "id"));
					if (secret_id >= 0) ap_locations[AP_SHORT_LOCATION (secret_id)].state |= (AP_LOC_SECRET);
				}
			}
			else if (!strcmp (k_loctype, "exits"))
			{
				const char* k_exitkey;
				json_t* v_exitdata;
				json_object_foreach (v_loctype, k_exitkey, v_exitdata)
				{
					uint64_t exit_id = json_integer_value (json_object_get (v_exitdata, "id"));
					if (exit_id >= 0) ap_locations[AP_SHORT_LOCATION (exit_id)].state |= (AP_LOC_EXIT);
				}
			}
		}
	}
}

static void set_available_locations (json_t* jobj)
{
	size_t i;
	json_t* v;
	json_array_foreach (jobj, i, v) 
	{
		ap_net_id_t loc_id = json_integer_value (v);
		ap_location_t short_id = AP_SHORT_LOCATION (loc_id);

		if (AP_VALID_LOCATION_ID (short_id))
		{
			ap_locations[short_id].state |= (AP_LOC_USED);
			g_array_append_val (scout_reqs, loc_id);
		}
	}
}

static void set_settings (json_t* jobj)
{
	g_hash_table_remove_all (ap_game_settings);
	const char* k_settingkey;
	json_t* v_settingdata;
	json_object_foreach (jobj, k_settingkey, v_settingdata)
	{
		GString* gs_key = g_string_new (k_settingkey);
		g_hash_table_insert (ap_game_settings, gs_key, json_deep_copy(v_settingdata));
		// set skill level
		if (!strcmp (k_settingkey, "difficulty")) {
			ap_skill = (int)json_integer_value (v_settingdata);
		}
		if (!strcmp (k_settingkey, "shell_recharge")) {
			ap_shell_recharge = (int)json_integer_value (v_settingdata);
		}
	}
}

static void set_used_levels (json_t* jobj)
{
	g_hash_table_remove_all (ap_used_level_unlocks);
	//ap_printf ("%s\n", json_dumps (jobj, JSON_DECODE_ANY));
	size_t i;
	json_t* v;
	json_array_foreach (jobj, i, v)
	{
		uint64_t* key = (uint64_t*) malloc (sizeof (uint64_t));
		if (key != NULL) *key = json_integer_value (v);
		uint8_t unlock_status = 1;
		g_hash_table_insert (ap_used_level_unlocks, key, &unlock_status);
	}
}

static void initialize_save_data (json_t* init_data)
{
	// TODO: Test this with data saved on the server
	if (init_data != NULL) {
		ap_game_state->dynamic_player = json_deep_copy (json_object_get (init_data, "player"));
		reached_goal = json_integer_value (json_object_get (ap_game_state->dynamic_player, "victory"));
	}
	else {
		//ap_game_state->dynamic_player = NULL;
		reached_goal = 0;
	}
}

static void init_trap_arr () {
	for (int i = 0; i < TRAPS_MAX; i++)
	{
		ap_active_traps[i] = 0;
	}
}

bool sync_wait_for_data (uint32_t timeout)
{
	char* serialized_save_data = NULL;
	json_t* save_data;
	save_data = NULL;
	uint32_t elapsed_ms = 0;
	clock_t start_time = clock ();

	while (AP_GetDataPackageStatus () != Synced) {
		AP_WebService ();

		clock_t current_time = clock ();
		elapsed_ms = (uint32_t)((double)(current_time - start_time) / CLOCKS_PER_SEC * 1000);
		if (elapsed_ms > timeout) {
			ap_error ("Timed out connecting to server.\n");
			exit (0);
			return 1;
		}
	}

	const char* prefix = AP_GetPrivateServerDataPrefix ()->str;
	const char* suffix = "_save_data";
	size_t total_length = strlen (prefix) + strlen (suffix) + 1;
	char* key = (char*)malloc (total_length);
	if (key == NULL) {
		ap_error ("Memory allocation failed\n");
		return 1;
	}
	strcpy (key, prefix);
	strcat (key, suffix);
	// Now fetch our save data
	struct AP_GetServerDataRequest save_req = {
		Pending,
		key,
		(void*)serialized_save_data,
		Raw
	}; 


	elapsed_ms = 0;
	start_time = clock ();

	AP_GetServerData (&save_req);
	while (save_req.status != Done)
	{
		AP_WebService ();

		clock_t current_time = clock ();
		elapsed_ms = (uint32_t)((double)(current_time - start_time) / CLOCKS_PER_SEC * 1000);
		if (elapsed_ms > timeout) {
			ap_error ("Timed out fetching save data from server.\n");
			exit (0);
			return 1;
		}
	}

	// Should have the id checksum from slot data by now, verify it matches our loaded ap_config.json	
	if (strcmp (json_string_value (json_object_get (ap_game_config, "checksum")), remote_id_checksum->str)) {
		ap_error ("Remote server item/location IDs don't match locally loaded configuration.");
		exit (0);
		return 1;
	}
	if (serialized_save_data != NULL)	save_data = json_loads (serialized_save_data, 0, &jerror);

	initialize_save_data (save_data);

	return 0;
}

static void set_id_checksums (json_t* jobj)
{
	remote_id_checksum = g_string_new (json_string_value (jobj));
}

static void init_dyn_player (json_t* dyn_player) {
	json_t* traps_obj = json_object ();
	json_t* player_obj = json_object ();
	json_object_set (dyn_player, "traps", traps_obj);
	json_object_set (dyn_player, "player", player_obj);
}

// AP Connection Funcs

DWORD WINAPI service_loop_thread (LPVOID lpParam) {
	service_loop ();
	return 0;
}

void AP_Initialize (json_t* game_config, ap_connection_settings_t connection)
{
	if (game_config == NULL || connection.mode == AP_DISABLED) return;

	ap_game_config = game_config;
	ap_game_id = json_integer_value (json_object_get (game_config, "game_id")) & AP_GAME_ID_MASK;

	ap_game_state = ap_state_new ();
	init_dyn_player (ap_game_state->dynamic_player);
	ap_set_default_inv ();
	init_trap_arr ();

	scouted_items = g_array_new (FALSE, FALSE, sizeof (gpointer));
	ap_item_info = g_hash_table_new (g_int64_hash, g_int64_equal);
	ap_goals = g_hash_table_new (g_string_hash, g_string_equal);
	ap_game_settings = g_hash_table_new (g_string_hash, g_string_equal);
	ap_used_levels = g_hash_table_new (g_string_hash, g_string_equal);
	ap_unlocked_levels = g_hash_table_new (g_string_hash, g_string_equal);
	ap_used_level_unlocks = g_hash_table_new (g_int64_hash, g_int64_equal);
	ap_ability_unlocks = g_hash_table_new (g_string_hash, g_string_equal);
	ap_automap_unlocks = g_hash_table_new (g_string_hash, g_string_equal);
	ap_keys_per_level = g_hash_table_new (g_string_hash, g_string_equal);
	ap_message_queue = g_queue_new ();
	scout_reqs = g_array_new (FALSE, FALSE, sizeof (gpointer));
	ap_inventory_flags = 0;

	init_location_table (json_object_get (game_config, "locations"));
	init_item_table (json_object_get (game_config, "items"));
	
	AP_SetClientVersion (AP_NetworkVersion_new (0, 5, 1));
	AP_SetDeathLinkSupported (TRUE);

	AP_Init (connection.ip, connection.port, connection.game, connection.player, connection.password);
	uint32_t timeout = 10000;
	ap_printf ("Connecting to AP Server.\n");
	uint32_t elapsed_ms = 0;
	clock_t start_time = clock ();

	while (AP_GetConnectionStatus () != Connected) {
		AP_WebService ();

		clock_t current_time = clock ();
		elapsed_ms = (uint32_t)((double)(current_time - start_time) / CLOCKS_PER_SEC * 1000);
		if (elapsed_ms > timeout) {
			ap_error ("Timed out connecting to server.\n");
			exit (0);
		}
	}
	
	ap_global_state = AP_CONNECTED;

	AP_SetItemClearCallback (&AP_ClearAllItems);
	AP_SetItemRecvCallback (&AP_ItemReceived);
	AP_SetLocationCheckedCallback (&AP_ExtLocationCheck);
	AP_SetLocationInfoCallback (&AP_LocationInfo);

	AP_RegisterSlotDataRawCallback ("goal", &set_goals);
	AP_RegisterSlotDataRawCallback ("locations", &set_available_locations);
	AP_RegisterSlotDataRawCallback ("settings", &set_settings);
	AP_RegisterSlotDataRawCallback ("levels", &set_used_levels);
	AP_RegisterSlotDataRawCallback ("checksum", &set_id_checksums);
	AP_Start ();

	if (sync_wait_for_data (timeout))
	{
		ap_error ("AP Server did not return slot data in time.\n");
		exit (0);
	}

	AP_SendLocationScouts (scout_reqs, FALSE);
	ap_printf ("Connection successful.\n");
	//AP_WebsocketTimer (50);
	if (!AP_WebsocketSulInit (50))
		ap_printf ("Failed to create Websocket Sul.\n");
	//service_loop ();
	HANDLE hThread = CreateThread (NULL, 0, service_loop_thread, NULL, 0, NULL);
	if (hThread == NULL) {
		ap_printf ("Error creating service loop thread\n");
		return;
	}

	ap_printf ("Waiting for server info.\n");
	while (!ap_received_scout_info) {}
	ap_printf ("Server info received.\n");

	ap_global_state = AP_INITIALIZED;
}

const char* ap_get_savedata_name () {
	return AP_GetPrivateServerDataPrefix ()->str;
}