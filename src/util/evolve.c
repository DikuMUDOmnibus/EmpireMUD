/* ************************************************************************
*   File: evolve.c                                        EmpireMUD 2.0b5 *
*  Usage: map updater for EmpireMUD                                       *
*                                                                         *
*  The program is called by the EmpireMUD server to evolve the world map  *
*  in a separate process, preventing constant lag in the game itself.     *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include "conf.h"
#include "sysdep.h"
#include <signal.h>
#include <math.h>
#include "structs.h"
#include "utils.h"
#include "db.h"

/**
* Table of contents:
*  LOCAL DATA
*  PROTOTYPES
*  EVOLUTIONS
*  MAIN
*  SECTOR I/O
*  MAP I/O
*  HELPER FUNCTIONS
*  RANDOM GENERATOR
*/


 //////////////////////////////////////////////////////////////////////////////
//// LOCAL DATA //////////////////////////////////////////////////////////////

// light version of map data for this program
struct map_t {
	room_vnum vnum;	
	int island_id;
	sector_vnum sector_type, base_sector, natural_sector;
	bitvector_t affects;
	
	struct map_t *next;	// next in land
};

struct map_t world[MAP_WIDTH][MAP_HEIGHT];	// world map grid
struct map_t *land = NULL;	// linked list of land tiles
sector_data *sector_table = NULL;	// sector hash table


// these are the arguments to shift_tile() to shift one tile in a direction, e.g. shift_tile(tile, shift_dir[dir][0], shift_dir[dir][1]) -- NUM_OF_DIRS
const int shift_dir[][2] = {
	{ 0, 1 },	// north
	{ 1, 0 },	// east
	{ 0, -1},	// south
	{-1, 0 },	// west
	{-1, 1 },	// nw
	{ 1, 1 },	// ne
	{-1, -1},	// sw
	{ 1, -1},	// se
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 },
	{ 0, 0 }
};


 //////////////////////////////////////////////////////////////////////////////
//// PROTOTYPES //////////////////////////////////////////////////////////////

int count_adjacent(struct map_t *tile, sector_vnum sect, bool count_original_sect);
struct evolution_data *get_evo_by_type(sector_vnum sect, int type);
void index_boot_sectors();
void load_base_map();
int map_distance(struct map_t *start, struct map_t *end);
int number(int from, int to);
bool sect_within_distance(struct map_t *tile, sector_vnum sect, int distance);
struct map_t *shift_tile(struct map_t *origin, int x_shift, int y_shift);

// this allows the inclusion of utils.h
void basic_mud_log(const char *format, ...) { }


 //////////////////////////////////////////////////////////////////////////////
//// EVOLUTIONS //////////////////////////////////////////////////////////////

/**
* attempt to evolve a single tile
*
* @param struct map_t *tile The tile to evolve.
* @param int nearby_distance The distance to count 'nearby' evos.
*/
void evolve_one(struct map_t *tile, int nearby_distance) {
	sector_data *original, *new_sect;
	struct evolution_data *evo;
	sector_vnum become, vnum;
	
	if (IS_SET(tile->affects, ROOM_AFF_NO_EVOLVE)) {
		return;	// never
	}
	
	// find sect
	vnum = tile->sector_type;
	HASH_FIND_INT(sector_table, &vnum, original);
	if (!original || vnum == BASIC_OCEAN || !GET_SECT_EVOS(original)) {
		return;	// no sector to evolve
	}
	
	
	printf("Trying %d (%d %s)...\n", tile->vnum, tile->sector_type, GET_SECT_NAME(original));
	
	// ok prepare to find one...
	become = NOTHING;
	
	// run some evolutions!
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_RANDOM))) {
		printf("hit random\n");
		become = evo->becomes;
	}
	
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_ADJACENT_ONE))) {
		if (count_adjacent(tile, evo->value, TRUE) >= 1) {
			become = evo->becomes;
		}
	}
	
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_NOT_ADJACENT))) {
		if (count_adjacent(tile, evo->value, TRUE) < 1) {
			become = evo->becomes;
		}
	}
	
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_ADJACENT_MANY))) {
		if (count_adjacent(tile, evo->value, TRUE) >= 6) {
			become = evo->becomes;
		}
	}
	
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_NEAR_SECTOR))) {
		if (sect_within_distance(tile, evo->value, nearby_distance)) {
			become = evo->becomes;
		}
	}
	
	if (become == NOTHING && (evo = get_evo_by_type(tile->sector_type, EVO_NOT_NEAR_SECTOR))) {
		if (!sect_within_distance(tile, evo->value, nearby_distance)) {
			become = evo->becomes;
		}
	}
	
	// DONE: now change it
	HASH_FIND_INT(sector_table, &become, new_sect);
	if (become != NOTHING && new_sect) {
		tile->sector_type = become;
	}
}


/**
* runs the evolutions on the whole map and writes the hint file
*
* @param int nearby_distance The distance to count 'nearby' evos.
*/
void evolve_map(int nearby_distance) {
	struct map_t *tile;
	sector_vnum old;
	int changed = 0;
	FILE *fl;
	
	if (!(fl = fopen(EVOLUTION_FILE, "w"))) {
		printf("ERROR: Unable to open evolution file %s\n", EVOLUTION_FILE);
		exit(1);
	}
	
	LL_FOREACH(land, tile) {
		old = tile->sector_type;
		
		// TODO remove this -- it's for testing
		if (tile->vnum != 119194) {
			continue;
		}
		
		evolve_one(tile, nearby_distance);
		
		if (tile->sector_type != old) {
			fprintf(fl, "%d %d %d\n", tile->vnum, old, tile->sector_type);
			++changed;
		}
	}
	
	fprintf(fl, "$\n");
	fclose(fl);
	
	printf("Changed %d tile%s\n", changed, PLURAL(changed));
}


 //////////////////////////////////////////////////////////////////////////////
//// MAIN ////////////////////////////////////////////////////////////////////

int main(int argc, char **argv) {
	struct map_t *tile;
	int num, nearby_distance, pid = 0;
	
	if (argc < 2 || argc > 3) {
		printf("Format: %s <nearby distance> [pid to signal]\n", argv[0]);
		exit(0);
	}
	
	nearby_distance = atoi(argv[1]);
	
	// determines if we will send a signal back to the mud
	if (argc == 3) {
		pid = atoi(argv[2]);
	}
	
	// load data
	index_boot_sectors();
	printf("Loaded %d sectors\n", HASH_COUNT(sector_table));
	load_base_map();
	LL_COUNT(land, tile, num);
	printf("Loaded %d land tiles\n", num);
	
	// evolve data
	evolve_map(nearby_distance);
	
	// signal back to the mud that we're done
	if (pid) {
		kill(pid, SIGUSR1);
	}
	
	return 0;
}


 //////////////////////////////////////////////////////////////////////////////
//// SECTOR I/O //////////////////////////////////////////////////////////////

/**
* This converts data file entries into bitvectors, where they may be written
* as "abdo" in the file, or as a number.
*
* - a-z are bits 1-26
* - A-Z are bits 27-52
* - !"#$%&'()*=, are bits 53-64
*
* @param char *flag The input string.
* @return bitvector_t The bitvector.
*/
bitvector_t asciiflag_conv(char *flag) {
	bitvector_t flags = 0;
	bool is_number = TRUE;
	char *p;

	for (p = flag; *p; ++p) {
		// skip numbers
		if (isdigit(*p)) {
			continue;
		}
		
		is_number = FALSE;
		
		if (islower(*p)) {
			flags |= BIT(*p - 'a');
		}
		else if (isupper(*p)) {
			flags |= BIT(26 + (*p - 'A'));
		}
		else {
			flags |= BIT(52 + (*p - '!'));
		}
	}

	if (is_number) {
		flags = strtoull(flag, NULL, 10);
	}

	return (flags);
}


/* read and allocate space for a '~'-terminated string from a given file */
char *fread_string(FILE * fl, char *error) {
	char buf[MAX_STRING_LENGTH], tmp[MAX_INPUT_LENGTH], *rslt;
	register char *point;
	int done = 0, length = 0, templength;

	*buf = '\0';

	do {
		if (!fgets(tmp, 512, fl)) {
			printf("ERROR: fread_string: format error at or near %s\n", error);
			exit(1);
		}
				
		// upgrade to allow ~ when not at the end
		point = strchr(tmp, '\0');
		
		// backtrack past any \r\n or trailing space or tab
		for (--point; *point == '\r' || *point == '\n' || *point == ' ' || *point == '\t'; --point);
		
		// look for a trailing ~
		if (*point == '~') {
			*point = '\0';
			done = 1;
		}
		else {
			strcpy(point+1, "\r\n");
		}

		templength = strlen(tmp);

		if (length + templength >= MAX_STRING_LENGTH) {
			printf("ERROR: fread_string: string too large (db.c)\n");
			printf("%s\n", error);
			exit(1);
		}
		else {
			strcat(buf + length, tmp);
			length += templength;
		}
	} while (!done);

	/* allocate space for the new string and copy it */
	if (strlen(buf) > 0) {
		CREATE(rslt, char, length + 1);
		strcpy(rslt, buf);
	}
	else {
		rslt = NULL;
	}

	return (rslt);
}


/*
 * get_line reads the next non-blank line off of the input stream.
 * The newline character is removed from the input.  Lines which begin
 * with '*' are considered to be comments.
 *
 * Returns the number of lines advanced in the file.
 */
int get_line(FILE *fl, char *buf) {
	char temp[256];
	int lines = 0;

	do {
		fgets(temp, 256, fl);
		if (feof(fl)) {
			return (0);
		}
		lines++;
	} while (*temp == '*' || *temp == '\n');
	
	if (temp[strlen(temp) - 1] == '\n') {
		temp[strlen(temp) - 1] = '\0';
	}
	strcpy(buf, temp);
	return (lines);
}


/**
* Read one sector from file.
*
* @param FILE *fl The open sector file
* @param sector_vnum vnum The sector vnum
*/
void parse_sector(FILE *fl, sector_vnum vnum) {
	char line[256], str_in[256], str_in2[256], char_in[2], error[256], *tmp;
	struct evolution_data *evo, *last_evo = NULL;
	sector_data *sect, *find;
	double dbl_in;
	int int_in[4];
		
	// for error messages
	sprintf(error, "sector vnum %d", vnum);
	
	// create
	CREATE(sect, sector_data, 1);
	sect->vnum = vnum;
	
	HASH_FIND_INT(sector_table, &vnum, find);
	if (find) {
		printf("WARNING: Duplicate sector vnum #%d\n", vnum);
		// but have to load it anyway to advance the file
	}
	else {
		HASH_ADD_INT(sector_table, vnum, sect);
	}
	
	// lines 1-2
	GET_SECT_NAME(sect) = fread_string(fl, error);
	GET_SECT_TITLE(sect) = fread_string(fl, error);
	
	// line 3: roadside, mapout, climate, movement, flags, build flags
	if (!get_line(fl, line) || sscanf(line, "'%c' %d %d %d %s %s", &char_in[0], &int_in[0], &int_in[1], &int_in[2], str_in, str_in2) != 6) {
		printf("ERROR: Format error in line 3 of %s\n", error);
		exit(1);
	}
	
	GET_SECT_ROADSIDE_ICON(sect) = char_in[0];
	GET_SECT_MAPOUT(sect) = int_in[0];
	GET_SECT_CLIMATE(sect) = int_in[1];
	GET_SECT_MOVE_LOSS(sect) = int_in[2];
	GET_SECT_FLAGS(sect) = asciiflag_conv(str_in);
	GET_SECT_BUILD_FLAGS(sect) = asciiflag_conv(str_in2);
		
	// optionals
	for (;;) {
		if (!get_line(fl, line)) {
			printf("ERROR: Format error in %s, expecting alphabetic flags\n", error);
			exit(1);
		}
		switch (*line) {
			case 'E': {	// evolution
				if (!get_line(fl, line) || sscanf(line, "%d %d %lf %d", &int_in[0], &int_in[1], &dbl_in, &int_in[2]) != 4) {
					printf("ERROR: Bad data in E line of %s\n", error);
					exit(1);
				}
				
				CREATE(evo, struct evolution_data, 1);
				evo->type = int_in[0];
				evo->value = int_in[1];
				evo->percent = dbl_in;
				evo->becomes = int_in[2];
				evo->next = NULL;
				
				if (last_evo) {
					last_evo->next = evo;
				}
				else {
					GET_SECT_EVOS(sect) = evo;
				}
				last_evo = evo;
				break;
			}
			
			case 'C': {	// commands -- unneeded
				tmp = fread_string(fl, error);
				free(tmp);
				break;
			}
			case 'D': {	// tile sets -- unneeded
				tmp = fread_string(fl, error);
				free(tmp);
				tmp = fread_string(fl, error);
				free(tmp);
				break;
			}
			
			case 'I': {	// interaction item -- unneeded
				// nothing to do -- these are 1-line types
				break;
			}
			case 'M': {	// mob spawn -- unneeded
				get_line(fl, line);	// 1 extra line
				break;
			}

			// end
			case 'S': {
				return;
			}
			
			default: {
				printf("ERROR: Format error in %s, expecting alphabetic flags\n", error);
				exit(1);
			}
		}
	}
}


/**
* Discrete load processes a data file, finds #VNUM records in it, and then
* sends them to the parser.
*
* @param FILE *fl The file to read.
* @param char *filename The name of the file, for error reporting.
*/
void discrete_load_sector(FILE *fl, char *filename) {
	any_vnum nr = -1, last;
	char line[256];
	
	for (;;) {
		if (!get_line(fl, line)) {
			if (nr == -1) {
				printf("ERROR: sector file %s is empty!\n", filename);
			}
			else {
				printf("ERROR: Format error in %s after sector #%d\n...expecting a new sector, but file ended!\n(maybe the file is not terminated with '$'?)\n", filename, nr);
			}
			exit(1);
		}
		
		if (*line == '$') {
			return;	// done
		}

		if (*line == '#') {	// new entry
			last = nr;
			if (sscanf(line, "#%d", &nr) != 1) {
				printf("ERROR: Format error after sector #%d\n", last);
				exit(1);
			}

			parse_sector(fl, nr);
		}
		else {
			printf("ERROR: Format error in sector file %s near sector #%d\n", filename, nr);
			printf("ERROR: ... offending line: '%s'\n", line);
			exit(1);
		}
	}
}


/**
* Loads all the sectors into memory from the index file.
*/
void index_boot_sectors(void) {
	char buf[MAX_STRING_LENGTH], filename[256];
	FILE *index, *db_file;
	
	sprintf(filename, "%s%s", SECTOR_PREFIX, INDEX_FILE);
	
	if (!(index = fopen(filename, "r"))) {
		printf("ERROR: opening index file '%s': %s\n", filename, strerror(errno));
		exit(1);
	}
	
	fscanf(index, "%s\n", buf);
	while (*buf != '$') {
		sprintf(filename, "%s%s", SECTOR_PREFIX, buf);
		if (!(db_file = fopen(filename, "r"))) {
			printf("ERROR: %s: %s\n", filename, strerror(errno));
			exit(1);
		}
		
		discrete_load_sector(db_file, buf);
		
		fclose(db_file);
		fscanf(index, "%s\n", buf);
	}
	fclose(index);
}


 //////////////////////////////////////////////////////////////////////////////
//// MAP I/O /////////////////////////////////////////////////////////////////


/**
* This loads the world array from file. This is optional.
*/
void load_base_map(void) {
	char line[256], line2[256], error_buf[MAX_STRING_LENGTH], *tmp;
	struct map_t *map, *last = NULL, *last_land = NULL;
	int var[7], x, y;
	FILE *fl;
	
	// init
	land = NULL;
	for (x = 0; x < MAP_WIDTH; ++x) {
		for (y = 0; y < MAP_HEIGHT; ++y) {
			world[x][y].vnum = (y * MAP_WIDTH) + x;
			world[x][y].island_id = NO_ISLAND;
			world[x][y].sector_type = NOTHING;
			world[x][y].base_sector = NOTHING;
			world[x][y].natural_sector = NOTHING;
			world[x][y].next = NULL;
		}
	}
	
	if (!(fl = fopen(WORLD_MAP_FILE, "r"))) {
		printf("ERROR: No world map file '%s' to evolve\n", WORLD_MAP_FILE);
		exit(0);
	}
	
	strcpy(error_buf, "map file");
	
	// optionals
	while (get_line(fl, line)) {
		if (*line == '$') {
			break;
		}
		
		// new room
		if (isdigit(*line)) {
			// x y island sect base natural crop
			if (sscanf(line, "%d %d %d %d %d %d %d", &var[0], &var[1], &var[2], &var[3], &var[4], &var[5], &var[6]) != 7) {
				log("Encountered bad line in world map file: %s", line);
				continue;
			}
			if (var[0] < 0 || var[0] >= MAP_WIDTH || var[1] < 0 || var[1] >= MAP_HEIGHT) {
				log("Encountered bad location in world map file: (%d, %d)", var[0], var[1]);
				continue;
			}
		
			map = &(world[var[0]][var[1]]);
			sprintf(error_buf, "map tile %d", map->vnum);
			
			map->island_id = var[2];
			map->sector_type = var[3];
			map->base_sector = var[4];
			map->natural_sector = var[5];
			// map->crop_type = var[6];
			
			// add to land map?
			if (map->sector_type != BASIC_OCEAN) {
				if (last_land) {
					last_land->next = map;
				}
				else {
					land = map;
				}
				last_land = map;
			}
			
			last = map;	// store in case of more data
		}
		else if (last) {
			switch (*line) {
				case 'E': {	// affects
					if (!get_line(fl, line2)) {
						printf("ERROR: Unable to get E line for map tile #%d\n", last->vnum);
						break;
					}
					last->affects = asciiflag_conv(line2);
					break;
				}
				
				// unneeded junk:
				case 'M':	// description
				case 'N':	// name
				case 'I': {	// icon
					tmp = fread_string(fl, error_buf);
					free(tmp);
					break;
				}
				case 'X':	// resource depletion
				case 'Y':	// tracks
				case 'Z': {	// extra data
					get_line(fl, line2);
					break;
				}
			}
		}
		else {
			printf("ERROR: Junk data found in base_map file: %s\n", line);
			exit(0);
		}
	}
	
	fclose(fl);
}


 //////////////////////////////////////////////////////////////////////////////
//// HELPER FUNCTIONS ////////////////////////////////////////////////////////

/**
* Counts how many adjacent tiles have the given sector type...
*
* @param struct map_t *tile The location to check.
* @param sector_vnum The sector vnum to find.
* @param bool count_original_sect If TRUE, also checks BASE_SECT
* @return int The number of matching adjacent tiles.
*/
int count_adjacent(struct map_t *tile, sector_vnum sect, bool count_original_sect) {
	int iter, count = 0;
	struct map_t *to_room;
	
	for (iter = 0; iter < NUM_2D_DIRS; ++iter) {
		to_room = shift_tile(tile, shift_dir[iter][0], shift_dir[iter][1]);
		
		if (to_room && (to_room->sector_type == sect || (count_original_sect && to_room->base_sector == sect))) {
			++count;
		}
	}
	
	return count;
}


/**
* This function takes a set of coordinates and finds another location in
* relation to them. It checks the wrapping boundaries of the map in the
* process.
*
* @param int start_x The initial X coordinate.
* @param int start_y The initial Y coordinate.
* @param int x_shift How much to shift X by (+/-).
* @param int y_shift How much to shift Y by (+/-).
* @param int *new_x A variable to bind the new X coord to.
* @param int *new_y A variable to bind the new Y coord to.
* @return bool TRUE if a valid location was found; FALSE if it's off the map.
*/
bool get_coord_shift(int start_x, int start_y, int x_shift, int y_shift, int *new_x, int *new_y) {
	// clear these
	*new_x = -1;
	*new_y = -1;
	
	if (start_x < 0 || start_x >= MAP_WIDTH || start_y < 0 || start_y >= MAP_HEIGHT) {
		// bad location
		return FALSE;
	}
	
	// process x
	start_x += x_shift;
	if (start_x < 0) {
		if (WRAP_X) {
			start_x += MAP_WIDTH;
		}
		else {
			return FALSE;	// off the map
		}
	}
	else if (start_x >= MAP_WIDTH) {
		if (WRAP_X) {
			start_x -= MAP_WIDTH;
		}
		else {
			return FALSE;	// off the map
		}
	}
	
	// process y
	start_y += y_shift;
	if (start_y < 0) {
		if (WRAP_Y) {
			start_y += MAP_HEIGHT;
		}
		else {
			return FALSE;	// off the map
		}
	}
	else if (start_y >= MAP_HEIGHT) {
		if (WRAP_Y) {
			start_y -= MAP_HEIGHT;
		}
		else {
			return FALSE;	// off the map
		}
	}
	
	// found a valid location
	*new_x = start_x;
	*new_y = start_y;
	return TRUE;
}


/**
* Gets an evolution of the given type, if its percentage passes. If more than
* one of a type exists, the first one that matches the percentage will be
* returned.
*
* @param sector_vnum sect The sector to check.
* @param int type The EVO_x type to get.
* @return struct evolution_data* The found evolution, or NULL.
*/
struct evolution_data *get_evo_by_type(sector_vnum sect, int type) {
	struct evolution_data *evo, *found = NULL;
	sector_data *st;
	
	if (sect == 10302) {
		printf("checking 10302: ");
	}
	
	HASH_FIND_INT(sector_table, &sect, st);
	if (!st) {
		return NULL;
		if (sect == 10302) {
			printf("no sect\n");
		}
	}
	
	// this iterates over matching types checks their percent chance until it finds one
	for (evo = GET_SECT_EVOS(st); evo && !found; evo = evo->next) {
		if (evo->type == type) {
			if (sect == 10302) {
				printf("found ");
			}
			if ((number(1, 10000) <= ((int) 100 * evo->percent))) {
				found = evo;
			}
		}
	}
	
	if (sect == 10302) {
		printf(" %d\n", found ? found->becomes : NOTHING);
	}
	
	return found;
}


// quick distance computation
int map_distance(struct map_t *start, struct map_t *end) {
	int dist;
	int x1 = MAP_X_COORD(start->vnum), x2 = MAP_X_COORD(end->vnum);
	int y1 = MAP_Y_COORD(start->vnum), y2 = MAP_Y_COORD(end->vnum);
	
	dist = ((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
	dist = (int) sqrt(dist);
	
	return dist;
}


/**
* This determines if tile is close enough to a given sect.
*
* @param struct map_t *tile
* @param sector_vnum sect Sector vnum
* @param int distance how far away to check
* @return bool TRUE if the sect is found
*/
bool sect_within_distance(struct map_t *tile, sector_vnum sect, int distance) {
	bool found = FALSE;
	struct map_t *shift;
	int x, y;
	
	for (x = -1 * distance; x <= distance && !found; ++x) {
		for (y = -1 * distance; y <= distance && !found; ++y) {
			shift = shift_tile(tile, x, y);
			if (shift && tile->sector_type == sect && map_distance(tile, shift) <= distance) {
				found = TRUE;
			}
		}
	}
	
	return found;
}


/**
* Find something relative to another tile.
* This function returns NULL if there is no valid shift.
*
* @param struct map_t *origin The start location
* @param int x_shift How far to move east/west
* @param int y_shift How far to move north/south
* @return struct map_t* The new location on the map, or NULL if the location would be off the map
*/
struct map_t *shift_tile(struct map_t *origin, int x_shift, int y_shift) {
	int x_coord, y_coord;
	
	// sanity?
	if (!origin) {
		return NULL;
	}
	
	if (get_coord_shift(MAP_X_COORD(origin->vnum), MAP_Y_COORD(origin->vnum), x_shift, y_shift, &x_coord, &y_coord)) {
		return &world[x_coord][y_coord];
	}
	return NULL;
}


 //////////////////////////////////////////////////////////////////////////////
//// RANDOM GENERATOR ////////////////////////////////////////////////////////

// borrowed from the EmpireMUD/CircleMUD base,
/* This program is public domain and was written by William S. England (Oct 1988) */

#define	mM  (unsigned long)2147483647
#define	qQ  (unsigned long)127773
#define	aA (unsigned int)16807
#define	rR (unsigned int)2836
static unsigned long seed;

void empire_srandom(unsigned long initial_seed) {
    seed = initial_seed;
}

unsigned long empire_random(void) {
	register int lo, hi, test;

	hi = seed/qQ;
	lo = seed%qQ;

	test = aA*lo - rR*hi;

	if (test > 0)
		seed = test;
	else
		seed = test+ mM;

	return (seed);
}

// core number generator
int number(int from, int to) {
	// shortcut -paul 12/9/2014
	if (from == to) {
		return from;
	}
	
	/* error checking in case people call number() incorrectly */
	if (from > to) {
		int tmp = from;
		from = to;
		to = tmp;
	}

	return ((empire_random() % (to - from + 1)) + from);
}
