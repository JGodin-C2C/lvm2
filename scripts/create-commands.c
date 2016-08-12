#include <asm/types.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>

/* required to include args.h */
#define ARG_COUNTABLE 0x00000001        /* E.g. -vvvv */
#define ARG_GROUPABLE 0x00000002        /* E.g. --addtag */

/* required to include args.h */
int yes_no_arg(void *cmd, void *av) { return 0; };
int activation_arg(void *cmd, void *av) { return 0; };
int cachemode_arg(void *cmd, void *av) { return 0; };
int discards_arg(void *cmd, void *av) { return 0; };
int mirrorlog_arg(void *cmd, void *av) { return 0; };
int size_kb_arg(void *cmd, void *av) { return 0; };
int size_mb_arg(void *cmd, void *av) { return 0; };
int size_mb_arg_with_percent(void *cmd, void *av) { return 0; };
int int_arg(void *cmd, void *av) { return 0; };
int int_arg_with_sign(void *cmd, void *av) { return 0; };
int int_arg_with_sign_and_percent(void *cmd, void *av) { return 0; };
int major_arg(void *cmd, void *av) { return 0; };
int minor_arg(void *cmd, void *av) { return 0; };
int string_arg(void *cmd, void *av) { return 0; };
int tag_arg(void *cmd, void *av) { return 0; };
int permission_arg(void *cmd, void *av) { return 0; };
int metadatatype_arg(void *cmd, void *av) { return 0; };
int units_arg(void *cmd, void *av) { return 0; };
int segtype_arg(void *cmd, void *av) { return 0; };
int alloc_arg(void *cmd, void *av) { return 0; };
int locktype_arg(void *cmd, void *av) { return 0; };
int readahead_arg(void *cmd, void *av) { return 0; };
int metadatacopies_arg(void *cmd, void *av) { return 0; };

struct opt_name {
	const char short_opt;
	char _padding[7];
	const char *long_opt;

	int (*fn) (void *cmd, void *av); /* NULL if option takes no arg */
	uint32_t unused1;
	uint32_t unused2;
};

/* enum for opt names */
enum {
#define arg(a, b, c, d, e, f) a ,
#include "args.h"
#undef arg
};

static struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f) {b, "", "--" c, d, e, f},
#include "args.h"
#undef arg
};

#define MAX_CMD_NAMES 64
struct cmd_name {
	const char *name;
};

static struct cmd_name cmd_names[MAX_CMD_NAMES] = {
	"lvchange",
	"lvconvert",
	"lvcreate",
	"lvdisplay",
	"lvextend",
	"lvmchange",
	"lvmconfig",
	"lvmdiskscan",
	"lvmsadc",
	"lvmsar",
	"lvreduce",
	"lvremove",
	"lvrename",
	"lvresize",
	"lvs",
	"lvscan",
	"pvchange",
	"pvresize",
	"pvck",
	"pvcreate",
	"pvdisplay",
	"pvmove",
	"pvremove",
	"pvs",
	"pvscan",
	"vgcfgbackup",
	"vgcfgrestore",
	"vgchange",
	"vgck",
	"vgconvert",
	"vgcreate",
	"vgdisplay",
	"vgexport",
	"vgextend",
	"vgimport",
	"vgimportclone",
	"vgmerge",
	"vgmknodes",
	"vgreduce",
	"vgremove",
	"vgrename",
	"vgs",
	"vgscan",
	"vgsplit",
	"end",
};

enum {
	VAL_TYPE_NONE       = 0,
	VAL_TYPE_BOOL       = 1 << 0,
	VAL_TYPE_NUM_ANY    = 1 << 1,
	VAL_TYPE_STR_ANY    = 1 << 2,
	VAL_TYPE_NUM_CONST  = 1 << 3,
	VAL_TYPE_STR_CONST  = 1 << 4,
	VAL_TYPE_STR_SET    = 1 << 5, /* a set of specific accepted string values */
	VAL_TYPE_PERCENT    = 1 << 6,
	VAL_TYPE_NAME_ANY   = 1 << 7,
	VAL_TYPE_NAME_PV    = 1 << 8,
	VAL_TYPE_NAME_VG    = 1 << 9,
	VAL_TYPE_NAME_LV    = 1 << 10,
	VAL_TYPE_TAG	    = 1 << 11,
	/* If needed, could add NUM_SET, with a defined set of accepted numbers. */
};

#define MAX_TYPE_NAMES 64
struct type_name {
	const char *name;
	int flag;
};

static struct type_name type_names[MAX_TYPE_NAMES] = {
	{ "None",           VAL_TYPE_NONE},
	{ "Bool",           VAL_TYPE_BOOL},
	{ "Number",         VAL_TYPE_NUM_ANY},
	{ "String",         VAL_TYPE_STR_ANY},
	{ "Name",           VAL_TYPE_NAME_ANY},
	{ "PV",             VAL_TYPE_NAME_PV},
	{ "VG",             VAL_TYPE_NAME_VG},
	{ "LV",             VAL_TYPE_NAME_LV},
	{ "Tag",            VAL_TYPE_TAG},
};

enum {
	VAL_LV_ANY	 = 0,
	VAL_LV_LINEAR    = 1 << 0,
	VAL_LV_STRIPED   = 1 << 1,
	VAL_LV_SNAPSHOT  = 1 << 2,
	VAL_LV_MIRROR    = 1 << 3,
	VAL_LV_RAID	 = 1 << 4,
	VAL_LV_RAID0	 = 1 << 5,
	VAL_LV_RAID1	 = 1 << 6,
	VAL_LV_RAID4	 = 1 << 7,
	VAL_LV_RAID5	 = 1 << 8,
	VAL_LV_RAID6	 = 1 << 9,
	VAL_LV_RAID10    = 1 << 10,
	VAL_LV_THIN	 = 1 << 11,
	VAL_LV_THINPOOL  = 1 << 12,
	VAL_LV_CACHE	 = 1 << 13,
	VAL_LV_CACHEPOOL = 1 << 14,
};

#define MAX_LV_NAMES 64
struct lv_name {
	const char *name;
	int flag;
};

static struct lv_name lv_names[MAX_LV_NAMES] = {
	{ "LV",           VAL_LV_ANY},
	{ "LV_linear",    VAL_LV_LINEAR},
	{ "LV_striped",   VAL_LV_STRIPED},
	{ "LV_snapshot",  VAL_LV_SNAPSHOT},
	{ "LV_mirror",    VAL_LV_MIRROR},
	{ "LV_raid",      VAL_LV_RAID},
	{ "LV_raid0",     VAL_LV_RAID0},
	{ "LV_raid1",     VAL_LV_RAID1},
	{ "LV_raid4",     VAL_LV_RAID4},
	{ "LV_raid5",     VAL_LV_RAID5},
	{ "LV_raid6",     VAL_LV_RAID6},
	{ "LV_raid10",    VAL_LV_RAID10},
	{ "LV_thin",      VAL_LV_THIN},
	{ "LV_thinpool",  VAL_LV_THINPOOL},
	{ "LV_cache",     VAL_LV_CACHE},
	{ "LV_cachepool", VAL_LV_CACHEPOOL},
};

/* def flags */
#define ARG_NEW 1
#define ARG_MAY_REPEAT 2

/*
 * An arg_def is a specification, defining possible
 * values that are accepted, either as an arg following
 * an option, or as a positional arg.
 */

#define MAX_STR_SET 16

struct arg_def {
	uint32_t types;		          
	uint32_t lv_types;		  
	uint64_t num;		          
	const char *str;		  
	const char *str_set[MAX_STR_SET]; 
	uint32_t flags;		          
};

/*
 * An arg_val is a description of one specific value,
 * plus the value itself, which was found as an arg
 * following an option or in a position.
 */

struct arg_val {
	uint32_t type;	      /* VAL_TYPE_ */
	uint32_t lv_type;	   /* VAL_LV_ (when type=NAME_LV) */
	union {
		int bool_val;
		int32_t  num_i32;
		uint32_t num_u32;
		int64_t  num_i64;
		uint64_t num_u64;
		const char *str_name;
		/* percent_type_t percent; */
	} val;
};

/*
 * An option arg:
 * an option (opt), a definition of what values are
 * accepted after that option (def), and the actual
 * value (val).
 */

struct opt_arg {
	int opt;             /* option, e.g. foo_ARG */
	struct arg_def def;  /* defines accepted values */
	struct arg_val *val; /* an actual value */
};

/*
 * A positional arg:
 * a position (pos), a definition of what values are
 * accepted in that position (def), and the actual
 * value (val).
 */

struct pos_arg {
	int pos;             /* position, e.g. first is 1 */
	struct arg_def def;  /* defines accepted values */
	struct arg_val *val; /* an actual value */
};
/*
 * A command prototype defines a commmand in terms
 * of what's required and what's optional.
 *
 * Two commands are unique when the combination of
 * name + required_opt_arg + required_pos_arg is unique.
 *
 * When the cmd struct holds a command prototype,
 * arg_def is used to define allowed values.
 *
 * When the cmd struct holds an actual command,
 * arg_val is used.
 */

#define MAX_OPT_ARGS 256
#define MAX_POS_ARGS 8

struct cmd {
	const char *name;

	/* required args following an --opt */
	struct opt_arg required_opt_args[MAX_OPT_ARGS];

	/* optional args following an --opt */
	struct opt_arg optional_opt_args[MAX_OPT_ARGS];

	/* required positional args */
	struct pos_arg required_pos_args[MAX_POS_ARGS];

	/* optional positional args */
	struct pos_arg optional_pos_args[MAX_POS_ARGS];

	/* indexes for adding entries to arrays above */
	int ro_count;
	int oo_count;
	int rp_count;
	int op_count;

	/* count current position including all pos args */
	int pos_count;
};


#define MAX_LINE 1024
#define MAX_LINE_ARGC 256

#define REQUIRED 1
#define OPTIONAL 0

struct oo_line {
	char *name;
	char *line;
};

#define MAX_CMDS 256
int cmd_count;
struct cmd cmd_array[MAX_CMDS];

#define MAX_OO_LINES 256
int oo_line_count;
struct oo_line oo_lines[MAX_OO_LINES];


static void add_optional_opt_line(struct cmd *cmd, int argc, char *argv[]);


static int opt_str_to_num(char *str)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++) {
		if (!opt_names[i].long_opt)
			continue;
		if (!strcmp(opt_names[i].long_opt, str))
			return i;
	}

	printf("Unknown opt str: %s\n", str);
	exit(1);
}

static int lv_type_str_to_num(char *str)
{
	char name[32] = { 0 };
	char *new;
	int i;

	/* compare the lv name before the _new suffix */

	strncpy(name, str, 31);
	if ((new = strstr(name, "_new")))
		*new = '\0';

	if (!strcmp(name, "LV"))
		return 0;

	for (i = 1; i < MAX_LV_NAMES; i++) {
		if (!lv_names[i].name)
			break;

		if (!strcmp(name, lv_names[i].name))
			return lv_names[i].flag;
	}

	printf("Unknown LV type: \"%s\" \"%s\"\n", name, str);
	exit(1);
}

static int type_str_to_num(char *str)
{
	char name[32] = { 0 };
	char *new;
	int i;

	/* compare the name before the _new suffix */

	strncpy(name, str, 31);
	if ((new = strstr(name, "_new")))
		*new = '\0';

	for (i = 0; i < MAX_TYPE_NAMES; i++) {
		if (!type_names[i].name)
			break;
		if (!strncmp(name, type_names[i].name, strlen(type_names[i].name)))
			return type_names[i].flag;
	}

	return 0;
}

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

static char *split_line(char *buf, int *argc, char **argv, char sep)
{
	char *p = buf, *rp = NULL;
	int i;

	argv[0] = p;

	for (i = 1; i < MAX_LINE_ARGC; i++) {
		p = strchr(buf, sep);
		if (!p)
			break;
		*p = '\0';

		argv[i] = p + 1;
		buf = p + 1;
	}
	*argc = i;

	/* we ended by hitting \0, return the point following that */
	if (!rp)
		rp = strchr(buf, '\0') + 1;

	return rp;
}

static const char *is_command_name(char *str)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!strcmp(cmd_names[i].name, "end"))
			break;
		if (!strcmp(cmd_names[i].name, str))
			return cmd_names[i].name;
	}
	return NULL;
}

static int is_opt_name(char *str)
{
	if (!strncmp(str, "--", 2))
		return 1;

	if ((str[0] == '-') && (str[1] != '-')) {
		printf("Options must be specified in long form: %s\n", str);
		exit(1);
	}

	return 0;
}

static int is_pos_name(char *str)
{
	if (!strncmp(str, "VG", 2))
		return 1;
	if (!strncmp(str, "LV", 2))
		return 1;
	if (!strncmp(str, "PV", 2))
		return 1;
	if (!strncmp(str, "Tag", 3))
		return 1;
	return 0;
}

static int is_oo_definition(char *str)
{
	if (!strncmp(str, "OO_", 3))
		return 1;
	return 0;
}

static int is_oo_line(char *str)
{
	if (!strncmp(str, "OO:", 3))
		return 1;
	return 0;
}

static int is_op_line(char *str)
{
	if (!strncmp(str, "OP:", 3))
		return 1;
	return 0;
}

/*
 * parse str for anything that can appear in a position,
 * like VG, VG|LV, VG|LV_linear|LV_striped, etc
 */

static void set_pos_def(struct cmd *cmd, const char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int type_num;
	int i;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		type_num = type_str_to_num(name);

		if (!type_num) {
			printf("Unknown pos arg: %s\n", name);
			exit(1);
		}

		def->types |= type_num;

		if ((type_num == VAL_TYPE_NAME_LV) && strstr(name, "_"))
			def->lv_types |= lv_type_str_to_num(name);

		if (strstr(name, "_new"))
			def->flags |= ARG_NEW;
	}
}

/*
 * parse str for anything that can follow --option
 */

static void set_opt_def(struct cmd *cmd, const char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int type_num;
	int i, j;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		type_num = type_str_to_num(name);

		if (!type_num) {
			/* a literal number or string */

			if (isdigit(name[0]))
				type_num = VAL_TYPE_NUM_CONST;

			else if (isalpha(name[0]))
				type_num = VAL_TYPE_STR_CONST;

			else {
				printf("Unknown opt arg: %s\n", name);
				exit(0);
			}
		}


		switch (type_num) {
		case VAL_TYPE_NONE:
			break;

		case VAL_TYPE_BOOL:
		case VAL_TYPE_NUM_ANY:
		case VAL_TYPE_STR_ANY:
		case VAL_TYPE_NAME_ANY:
		case VAL_TYPE_NAME_PV:
		case VAL_TYPE_NAME_VG:
		case VAL_TYPE_TAG:
			def->types |= type_num;
			break;

		case VAL_TYPE_NUM_CONST:
			def->types |= type_num;
			def->num = (uint64_t)atoi(name);
			break;

		case VAL_TYPE_STR_CONST:
			if (def->types & VAL_TYPE_STR_CONST) {
				def->types &= ~VAL_TYPE_STR_CONST;
				def->types |= VAL_TYPE_STR_SET;
				def->str_set[0] = def->str;
				def->str = NULL;
			}

			if (def->types & VAL_TYPE_STR_SET) {
				for (j = 0; j < MAX_STR_SET; j++) {
					if (def->str_set[j])
						continue;
					def->str_set[j] = strdup(name);
					break;
				}
			} else {
				def->types |= type_num;
				def->str = strdup(name);
			}
			break;

		case VAL_TYPE_NAME_LV:
			def->types |= type_num;
			if (strstr(name, "_"))
				def->lv_types |= lv_type_str_to_num(name);
			break;
		}

		if ((type_num == VAL_TYPE_NAME_VG) ||
		    (type_num == VAL_TYPE_NAME_LV) ||
		    (type_num == VAL_TYPE_NAME_PV)) {
			if (strstr(name, "_new"))
				def->flags |= ARG_NEW;
		}
	}
}


/*
 * OO_FOO: --opt1 ...
 *
 * oo->name = "OO_FOO";
 * oo->line = "--opt1 ...";
 */

static void add_oo_definition_line(const char *name, const char *line)
{
	struct oo_line *oo;
	char *colon;
	char *start;

	oo = &oo_lines[oo_line_count++];
	oo->name = strdup(name);

	if ((colon = strstr(oo->name, ":")))
		*colon = '\0';
	else {
		printf("invalid OO definition\n");
		exit(1);
	}

	start = strstr(line, ":") + 2;
	oo->line = strdup(start);
}

/* when OO_FOO: continues on multiple lines */

static void append_oo_definition_line(const char *new_line)
{
	struct oo_line *oo;
	char *old_line;
	char *line;
	int len;

	oo = &oo_lines[oo_line_count-1];

	old_line = oo->line;

	/* +2 = 1 space between old and new + 1 terminating \0 */
	len = strlen(old_line) + strlen(new_line) + 2;
	line = malloc(len);

	strcat(line, old_line);
	strcat(line, " ");
	strcat(line, new_line);

	free(oo->line);
	oo->line = line;
}

static void include_optional_opt_args(struct cmd *cmd, char *str)
{
	char *name;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;
	int i;

	for (i = 0; i < oo_line_count; i++) {
		name = oo_lines[i].name;
		if (!strncmp(name, str, strlen(name))) {
			line = strdup(oo_lines[i].line);
			if (!line)
				exit(1);
			split_line(line, &line_argc, line_argv, ' ');
			add_optional_opt_line(cmd, line_argc, line_argv);
			free(line);
		}
	}
}

static void add_opt_arg(struct cmd *cmd, char *str, int *takes_arg, int required)
{
	char *comma;
	int opt;

	/* opt_arg.opt set here */
	/* opt_arg.def will be set in update_prev_opt_arg() if needed */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	opt = opt_str_to_num(str);

	if (required)
		cmd->required_opt_args[cmd->ro_count++].opt = opt;
	else
		cmd->optional_opt_args[cmd->oo_count++].opt = opt;

	*takes_arg = opt_names[opt].fn ? 1 : 0;
}

static void update_prev_opt_arg(struct cmd *cmd, char *str, int required)
{
	struct arg_def def = { 0 };
	char *comma;

	/* opt_arg.def set here */
	/* opt_arg.opt was previously set in add_opt_arg() when --foo was read */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	set_opt_def(cmd, str, &def);

	if (required)
		cmd->required_opt_args[cmd->ro_count-1].def = def;
	else
		cmd->optional_opt_args[cmd->oo_count-1].def = def;
}

static void add_pos_arg(struct cmd *cmd, char *str, int required)
{
	struct arg_def def = { 0 };

	/* pos_arg.pos and pos_arg.def are set here */

	set_pos_def(cmd, str, &def);

	if (required) {
		cmd->required_pos_args[cmd->rp_count].pos = cmd->pos_count++;
		cmd->required_pos_args[cmd->rp_count].def = def;
		cmd->rp_count++;
	} else {
		cmd->optional_pos_args[cmd->op_count].pos = cmd->pos_count++;;
		cmd->optional_pos_args[cmd->op_count].def = def;
		cmd->op_count++;
	}
}

/* process something that follows a pos arg, which is not a new pos arg */

static void update_prev_pos_arg(struct cmd *cmd, char *str, int required)
{
	struct arg_def *def;

	/* a previous pos_arg.def is modified here */

	if (required)
		def = &cmd->required_pos_args[cmd->rp_count-1].def;
	else
		def = &cmd->optional_pos_args[cmd->op_count-1].def;

	if (!strcmp(str, "..."))
		def->flags |= ARG_MAY_REPEAT;
	else {
		printf("Unknown pos arg: %s\n", str);
		exit(1);
	}
}

/* process what follows OO:, which are optional opt args */

static void add_optional_opt_line(struct cmd *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, OPTIONAL);
		else if (!strncmp(argv[i], "OO_", 3))
			include_optional_opt_args(cmd, argv[i]);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], OPTIONAL);
		else
			printf("Can't parse argc %d argv %s\n", i, argv[i]);
	}
}

/* process what follows OP:, which are optional pos args */

static void add_optional_pos_line(struct cmd *cmd, int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OP:", 3))
			continue;
		if (is_pos_name(argv[i]))
			add_pos_arg(cmd, argv[i], OPTIONAL);
		else
			update_prev_pos_arg(cmd, argv[i], OPTIONAL);
	}
}

/* process what follows command_name, which are required opt/pos args */

static void add_required_line(struct cmd *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg;
	int prev_was_opt = 0, prev_was_pos = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {
		if (is_opt_name(argv[i])) {
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;
		} else if (prev_was_opt && takes_arg) {
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;
		} else if (is_pos_name(argv[i])) {
			add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;
		} else if (prev_was_pos) {
			update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else
			printf("Can't parse argc %d argv %s\n", i, argv[i]);

	}
}

static void print_def(struct arg_def *def)
{
	int sep = 0;
	int i;

	if (def->types & VAL_TYPE_BOOL) {
		if (sep) printf("|");
		printf("Bool");
		sep = 1;
	}

	if (def->types & VAL_TYPE_NUM_ANY) {
		if (sep) printf("|");
		printf("Number");
		sep = 1;
	}

	if (def->types & VAL_TYPE_STR_ANY) {
		if (sep) printf("|");
		printf("String");
		sep = 1;
	}

	if (def->types & VAL_TYPE_NAME_ANY) {
		if (sep) printf("|");
		printf("Name");
		sep = 1;
	}

	if (def->types & VAL_TYPE_NAME_PV) {
		if (sep) printf("|");
		printf("PV");
		if (def->flags & ARG_NEW)
			printf("_new");
		sep = 1;
	}

	if (def->types & VAL_TYPE_NAME_VG) {
		if (sep) printf("|");
		printf("VG");
		if (def->flags & ARG_NEW)
			printf("_new");
		sep = 1;
	}

	if (def->types & VAL_TYPE_NAME_LV) {
		if (!def->lv_types) {
			if (sep) printf("|");
			printf("LV");
			sep = 1;
		} else {
			for (i = 0; i < MAX_LV_NAMES; i++) {
				if (def->lv_types & lv_names[i].flag) {
					if (sep) printf("|");
					printf("%s", lv_names[i].name);
					sep = 1;
				}
			}
		}
		if (def->flags & ARG_NEW)
			printf("_new");
	}

	if (def->types & VAL_TYPE_TAG) {
		if (sep) printf("|");
		printf("Tag");
		sep = 1;
	}

	if (def->types & VAL_TYPE_STR_CONST) {
		if (sep) printf("|");
		printf("%s", def->str);
		sep = 1;
	}

	if (def->types & VAL_TYPE_NUM_CONST) {
		if (sep) printf("|");
		printf("%llu", def->num);
		sep = 1;
	}

	if (def->types & VAL_TYPE_STR_SET) {
		for (i = 0; i < MAX_STR_SET; i++) {
			if (def->str_set[i]) {
				if (sep) printf("|");
				printf("%s", def->str_set[i]);
				sep = 1;
			}
		}
	}

	if (def->flags & ARG_MAY_REPEAT)
		printf(" ...");

	if (sep)
		printf(" ");
}

void print_data_as_input(void)
{
	struct cmd *cmd;
	int i, ro, rp, oo, op;

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];
		printf("%s ", cmd->name);

		if (cmd->ro_count) {
			for (ro = 0; ro < cmd->ro_count; ro++) {
				printf("%s ", opt_names[cmd->required_opt_args[ro].opt].long_opt);
				if (cmd->required_opt_args[ro].def.types)
					print_def(&cmd->required_opt_args[ro].def);
			}
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				/* printf("%d ", cmd->required_pos_args[rp].pos); */
				if (cmd->required_pos_args[rp].def.types)
					print_def(&cmd->required_pos_args[rp].def);
			}
		}

		if (cmd->oo_count) {
			printf("\n");
			printf("OO: ");
			for (oo = 0; oo < cmd->oo_count; oo++) {
				printf("%s ", opt_names[cmd->optional_opt_args[oo].opt].long_opt);
				if (cmd->optional_opt_args[oo].def.types)
					print_def(&cmd->optional_opt_args[oo].def);
			}
		}

		if (cmd->op_count) {
			printf("\n");
			printf("OP: ");
			for (op = 0; op < cmd->op_count; op++) {
				/* printf("%d ", cmd->optional_pos_args[op].pos); */
				if (cmd->optional_pos_args[op].def.types)
					print_def(&cmd->optional_pos_args[op].def);
			}
		}

		printf("\n\n");
	}
}

static char *opt_to_enum_str(int opt)
{
	static char buf[64] = { 0 };

	strcpy(buf, opt_names[opt].long_opt + 2);
	strcat(buf, "_ARG");
	return buf;
}

static char *types_to_flags_str(int types)
{
	static char buf[128];
	int or = 0;

	memset(buf, 0, sizeof(buf));

	if (types & VAL_TYPE_BOOL) {
		strcat(buf, "VAL_TYPE_BOOL");
		or = 1;
	}

	if (types & VAL_TYPE_NUM_ANY) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NUM_ANY");
		or = 1;
	}

	if (types & VAL_TYPE_STR_ANY) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_STR_ANY");
		or = 1;
	}

	if (types & VAL_TYPE_NUM_CONST) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NUM_CONST");
		or = 1;
	}

	if (types & VAL_TYPE_STR_CONST) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_STR_CONST");
		or = 1;
	}

	if (types & VAL_TYPE_STR_SET) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_STR_SET");
		or = 1;
	}

	if (types & VAL_TYPE_NAME_ANY) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NAME_ANY");
		or = 1;
	}

	if (types & VAL_TYPE_NAME_PV) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NAME_PV");
		or = 1;
	}

	if (types & VAL_TYPE_NAME_VG) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NAME_VG");
		or = 1;
	}

	if (types & VAL_TYPE_NAME_LV) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_NAME_LV");
		or = 1;
	}

	if (types & VAL_TYPE_TAG) {
		if (or) strcat(buf, "|");
		strcat(buf, "VAL_TYPE_TAG");
		or = 1;
	}

	return buf;
}

static char *lv_types_to_flags_str(int lv_types)
{
	static char buf_lv_types[128];
	int or = 0;

	memset(buf_lv_types, 0, sizeof(buf_lv_types));

	if (lv_types & VAL_LV_LINEAR) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_LINEAR");
		or = 1;
	}

	if (lv_types & VAL_LV_STRIPED) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_STRIPED");
		or = 1;
	}

	if (lv_types & VAL_LV_SNAPSHOT) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_SNAPSHOT");
		or = 1;
	}

	if (lv_types & VAL_LV_MIRROR) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_MIRROR");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID0) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID0");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID1) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID1");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID4) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID4");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID5) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID5");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID6) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID6");
		or = 1;
	}

	if (lv_types & VAL_LV_RAID10) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_RAID10");
		or = 1;
	}

	if (lv_types & VAL_LV_THIN) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_THIN");
		or = 1;
	}

	if (lv_types & VAL_LV_THINPOOL) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_THINPOOL");
		or = 1;
	}

	if (lv_types & VAL_LV_CACHE) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_CACHE");
		or = 1;
	}

	if (lv_types & VAL_LV_CACHEPOOL) {
		if (or) strcat(buf_lv_types, "|");
		strcat(buf_lv_types, "VAL_LV_CACHEPOOL");
		or = 1;
	}

	return buf_lv_types;
}

static char *flags_to_str(int flags)
{
	static char buf_flags[32];

	memset(buf_flags, 0, sizeof(buf_flags));

	if (flags & ARG_MAY_REPEAT)
		strcat(buf_flags, "ARG_MAY_REPEAT");
	if (flags & ARG_NEW)
		strcat(buf_flags, "ARG_NEW");

	return buf_flags;
}

void print_data_as_c(void)
{
	struct cmd *cmd;
	int i, j, ro, rp, oo, op;

	printf("#define CMD_COUNT %d\n", cmd_count);
	printf("struct cmd cmd_array[CMD_COUNT];\n");
	printf("\n");

	printf("void define_cmds(void) {\n");

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		printf("cmd_array[%d].name = \"%s\";\n", i, cmd->name);

		if (cmd->ro_count) {
			for (ro = 0; ro < cmd->ro_count; ro++) {
				printf("cmd_array[%d].required_opt_args[%d].opt = %s;\n",
					i, ro, opt_to_enum_str(cmd->required_opt_args[ro].opt));

				if (!cmd->required_opt_args[ro].def.types)
					continue;

				printf("cmd_array[%d].required_opt_args[%d].def.types = %s;\n",
					i, ro, types_to_flags_str(cmd->required_opt_args[ro].def.types));

				if (cmd->required_opt_args[ro].def.lv_types)
					printf("cmd_array[%d].required_opt_args[%d].def.lv_types = %s;\n",
						i, ro, lv_types_to_flags_str(cmd->required_opt_args[ro].def.lv_types));
				if (cmd->required_opt_args[ro].def.types & VAL_TYPE_NUM_CONST) 
					printf("cmd_array[%d].required_opt_args[%d].def.num = %d;\n",
						i, ro, cmd->required_opt_args[ro].def.num);
				if (cmd->required_opt_args[ro].def.flags)
					printf("cmd_array[%d].required_opt_args[%d].def.flags = %s;\n",
						i, ro, flags_to_str(cmd->required_opt_args[ro].def.flags));
				if (cmd->required_opt_args[ro].def.types & VAL_TYPE_STR_CONST)
					printf("cmd_array[%d].required_opt_args[%d].def.str = \"%s\";\n",
						i, ro, cmd->required_opt_args[ro].def.str ?: "NULL");
				if (cmd->required_opt_args[ro].def.types & VAL_TYPE_STR_SET) {
					for (j = 0; j < MAX_STR_SET; j++) {
						if (cmd->required_opt_args[ro].def.str_set[j])
							printf("cmd_array[%d].required_opt_args[%d].def.str_set[%d] = %s;\n",
								i, ro, j, cmd->required_opt_args[ro].def.str_set[j] ?: "NULL");
					}
				}
			}
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				printf("cmd_array[%d].required_pos_args[%d].pos = %d;\n",
					i, rp, cmd->required_pos_args[rp].pos);

				if (!cmd->required_pos_args[rp].def.types)
					continue;

				printf("cmd_array[%d].required_pos_args[%d].def.types = %s;\n",
					i, rp, types_to_flags_str(cmd->required_pos_args[rp].def.types));

				if (cmd->required_pos_args[rp].def.lv_types)
					printf("cmd_array[%d].required_pos_args[%d].def.lv_types = %s;\n",
						i, rp, lv_types_to_flags_str(cmd->required_pos_args[rp].def.lv_types));
				if (cmd->required_pos_args[rp].def.types & VAL_TYPE_NUM_CONST)
					printf("cmd_array[%d].required_pos_args[%d].def.num = %d;\n",
						i, rp, cmd->required_pos_args[rp].def.num);
				if (cmd->required_pos_args[rp].def.flags)
					printf("cmd_array[%d].required_pos_args[%d].def.flags = %s;\n",
						i, rp, flags_to_str(cmd->required_pos_args[rp].def.flags));
				if (cmd->required_pos_args[rp].def.types & VAL_TYPE_STR_CONST)
					printf("cmd_array[%d].required_pos_args[%d].def.str = \"%s\";\n",
						i, rp, cmd->required_pos_args[rp].def.str ?: "NULL");
				if (cmd->required_pos_args[rp].def.types & VAL_TYPE_STR_SET) {
					for (j = 0; j < MAX_STR_SET; j++) {
						if (cmd->required_pos_args[rp].def.str_set[j])
							printf("cmd_array[%d].required_pos_args[%d].def.str_set[%d] = \"%s\";\n",
								i, rp, j, cmd->required_pos_args[rp].def.str_set[j] ?: "NULL");
					}
				}
			}
		}

		if (cmd->oo_count) {
			for (oo = 0; oo < cmd->oo_count; oo++) {
				printf("cmd_array[%d].optional_opt_args[%d].opt = %s;\n",
					i, oo, opt_to_enum_str(cmd->optional_opt_args[oo].opt));

				if (!cmd->optional_opt_args[oo].def.types)
					continue;

				printf("cmd_array[%d].optional_opt_args[%d].def.types = %s;\n",
					i, oo, types_to_flags_str(cmd->optional_opt_args[oo].def.types));

				if (cmd->optional_opt_args[oo].def.lv_types)
					printf("cmd_array[%d].optional_opt_args[%d].def.lv_types = %s;\n",
						i, oo, lv_types_to_flags_str(cmd->optional_opt_args[oo].def.lv_types));
				if (cmd->optional_opt_args[oo].def.types & VAL_TYPE_NUM_CONST) 
					printf("cmd_array[%d].optional_opt_args[%d].def.num = %d;\n",
						i, oo, cmd->optional_opt_args[oo].def.num);
				if (cmd->optional_opt_args[oo].def.flags)
					printf("cmd_array[%d].optional_opt_args[%d].def.flags = %s;\n",
						i, oo, flags_to_str(cmd->optional_opt_args[oo].def.flags));
				if (cmd->optional_opt_args[oo].def.types & VAL_TYPE_STR_CONST)
					printf("cmd_array[%d].optional_opt_args[%d].def.str = \"%s\";\n",
						i, oo, cmd->optional_opt_args[oo].def.str ?: "NULL");
				if (cmd->optional_opt_args[oo].def.types & VAL_TYPE_STR_SET) {
					for (j = 0; j < MAX_STR_SET; j++) {
						if (cmd->optional_opt_args[oo].def.str_set[j])
							printf("cmd_array[%d].optional_opt_args[%d].def.str_set[%d] = \"%s\";\n",
								i, oo, j, cmd->optional_opt_args[oo].def.str_set[j] ?: "NULL");
					}
				}
			}
		}

		if (cmd->op_count) {
			for (op = 0; op < cmd->op_count; op++) {
				printf("cmd_array[%d].optional_pos_args[%d].pos = %d;\n",
					i, op, cmd->optional_pos_args[op].pos);

				if (!cmd->optional_pos_args[op].def.types)
					continue;

				printf("cmd_array[%d].optional_pos_args[%d].def.types = %s;\n",
					i, op, types_to_flags_str(cmd->optional_pos_args[op].def.types));

				if (cmd->optional_pos_args[op].def.lv_types)
					printf("cmd_array[%d].optional_pos_args[%d].def.lv_types = %s;\n",
						i, op, lv_types_to_flags_str(cmd->optional_pos_args[op].def.lv_types));
				if (cmd->optional_pos_args[op].def.types & VAL_TYPE_NUM_CONST)
					printf("cmd_array[%d].optional_pos_args[%d].def.num = %d;\n",
						i, op, cmd->optional_pos_args[op].def.num);
				if (cmd->optional_pos_args[op].def.flags)
					printf("cmd_array[%d].optional_pos_args[%d].def.flags = %s;\n",
						i, op, flags_to_str(cmd->optional_pos_args[op].def.flags));
				if (cmd->optional_pos_args[op].def.types & VAL_TYPE_STR_CONST)
					printf("cmd_array[%d].optional_pos_args[%d].def.str = \"%s\";\n",
						i, op, cmd->optional_pos_args[op].def.str ?: "NULL");
				if (cmd->optional_pos_args[op].def.types & VAL_TYPE_STR_SET) {
					for (j = 0; j < MAX_STR_SET; j++) {
						if (cmd->optional_pos_args[op].def.str_set[j])
							printf("cmd_array[%d].optional_pos_args[%d].def.str_set[%d] = \"%s\";\n",
								i, op, j, cmd->optional_pos_args[op].def.str_set[j] ?: "NULL");
					}
				}
			}
		}

		printf("\n");
	}

	printf("} /* define_cmds */\n");
}

void print_command_list(void)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		printf("%s\n", cmd_names[i].name);
	}
}

void print_option_list(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		printf("%s\n", opt_names[i].long_opt);
}

static void print_help(int argc, char *argv[])
{
	int i;

	printf("input file format\n");
	printf("\n");
	printf("command --required_opt required_opt_arg required_pos_arg\n");
	printf("OO: --optional_opt optional_opt_arg, --optional_opt optional_opt_arg\n");
	printf("OP: optional_pos_arg optional_pos_arg\n");
	printf("\n");
	printf("opt_arg and pos_arg can be:\n");
	printf(". Bool, Number, String, PV, VG, LV, Tag\n");
	printf(". The suffix _new can be added to PV, VG, or LV to indicate\n");
	printf("  the object does not yet exist.\n");
	printf(". The arg can be multiple types separated by |, e.g. VG|LV|Tag\n");
	printf(". The arg can be specified as a literal number, e.g. 1\n");
	printf(". The arg can be specified as a literal string, e.g. --type raid1\n");
	printf(". The arg can be a set of literal strings, e.g. --cachemode writethrough|writeback\n");
	printf(". A pos arg followed by ... is repeatable, e.g. LV ...\n");
	printf(". LVs can be further specified with a type as:\n");
	printf("  ");
	for (i = 1; i < MAX_LV_NAMES; i++) {
		if (!lv_names[i].name)
			break;
		printf("%s ", lv_names[i].name);
	}
	printf("\n");
	printf("\n");
	printf("To define and reuse a common set of options:\n");
	printf("OO_NAME: --optional_opt optional_opt_arg, --optional_opt optional_opt_arg\n");
	printf("Include OO_NAME in OO lists or other OO_NAME lists.\n");
	printf("\n");
	printf("Examples\n");
	printf("OO_ALL: --help, --test, --debug, --verbose\n");
	printf("OO_VGCHANGE: --autobackup Bool, OO_ALL\n");
	printf("OO_LVCONVERT: --cachepolicy writethrough|writeback, OO_ALL\n");
	printf("\n");
	printf("vgchange --activate n|y|ey|sy\n");
	printf("OO: OO_VGCHANGE\n");
	printf("OP: VG|Tag ...\n");
	printf("\n");
	printf("lvconvert --type raid1 LV_linear\n");
	printf("OO: --mirrors Number, OO_LVCONVERT\n");
	printf("\n");
	printf("lvconvert --type thin --thinpool LV_thinpool LV_linear|LV_striped|LV_raid\n");
	printf("OO: --originname LV_new, OO_LVCONVERT\n");
}

int main(int argc, char *argv[])
{
	char *outputformat = NULL;
	char *inputfile = NULL;
	FILE *file;
	struct cmd *cmd;
	char line[MAX_LINE];
	char line_orig[MAX_LINE];
	const char *name;
	char *line_argv[MAX_LINE_ARGC];
	char *n;
	int line_argc;
	int prev_was_oo_def = 0;
	int prev_was_oo = 0;
	int prev_was_op = 0;

	if (argc < 2) {
		printf("%s --output <format> | --inputfile <filename>\n", argv[0]);
		printf("output formats:\n");
		printf("input - same format as the input file (default).\n");
		printf("struct - C structs.\n");
		return 0;
	}

        static struct option long_options[] = {
		{"help",            no_argument,       0, 'h' },
                {"outputformat",    required_argument, 0, 'o' },
                {"inputfile",       required_argument, 0, 'f' },
                {0, 0, 0, 0 }
        };

        while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "ho:f:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case '0':
			break;
		case 'h':
                        print_help(argc, argv);
			exit(EXIT_SUCCESS);
		case 'o':
			outputformat = strdup(optarg);
			break;
		case 'f':
			inputfile = strdup(optarg);
			break;
		}
	}

	if (!inputfile) {
		printf("Missing filename.\n");
		return 0;
	}

	if (!(file = fopen(inputfile, "r"))) {
		printf("Cannot open %s\n", argv[1]);
		return -1;
	}

	while (fgets(line, MAX_LINE, file)) {
		if (line[0] == '#')
			continue;
		if (line[0] == '\n')
			continue;

		if ((n = strchr(line, '\n')))
			*n = '\0';

		memcpy(line_orig, line, sizeof(line));
		split_line(line, &line_argc, line_argv, ' ');

		if (!line_argc)
			continue;

		/* command ... */
		if ((name = is_command_name(line_argv[0]))) {
			if (cmd_count >= MAX_CMDS) {
				printf("MAX_CMDS too small\n");
				return -1;
			}
			cmd = &cmd_array[cmd_count++];
			cmd->name = name;
			cmd->pos_count = 1;
			add_required_line(cmd, line_argc, line_argv);
			continue;
		}

		/* OO_FOO: ... */
		if (is_oo_definition(line_argv[0])) {
			add_oo_definition_line(line_argv[0], line_orig);
			prev_was_oo_def = 1;
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* OO: ... */
		if (is_oo_line(line_argv[0])) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 1;
			prev_was_op = 0;
			continue;
		}

		/* OP: ... */
		if (is_op_line(line_argv[0])) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 0;
			prev_was_op = 1;
			continue;
		}

		/* handle OO_FOO:, OO:, OP: continuing on multiple lines */

		if (prev_was_oo_def) {
			append_oo_definition_line(line_orig);
			continue;
		}

		if (prev_was_oo) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			continue;
		}

		if (prev_was_op) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			continue;
		}
	}

	fclose(file);

	if (!outputformat)
		print_data_as_input();
	else if (!strcmp(outputformat, "struct"))
		print_data_as_c();
	else
		print_data_as_input();
}

