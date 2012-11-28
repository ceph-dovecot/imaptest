/* Copyright (c) 2007-2008 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "imap-parser.h"
#include "test-parser.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#define DEFAULT_MBOX_FNAME "default.mbox"

struct test_parser {
	pool_t pool;
	const char *dir, *default_mbox_path;

	struct imap_arg *reply_ok, *reply_no, *reply_bad, *reply_any;
	struct test_command *cur_cmd;
	ARRAY_TYPE(test) tests;
};

static bool
test_parse_header_line(struct test_parser *parser, struct test *test,
		       const char *line, const char **error_r)
{
	const char *key, *value;

	value = strchr(line, ':');
	if (value == NULL) {
		*error_r = "Missing ':'";
		return FALSE;
	}

	for (key = value; key[-1] == ' '; key--) ;
	key = t_str_lcase(t_strdup_until(line, key));
	for (value++; *value == ' '; value++) ;

	if (strcmp(key, "capabilities") == 0) {
		test->required_capabilities = (const char *const *)
			p_strsplit_spaces(parser->pool, value, " ");
		return TRUE;
	}
	if (strcmp(key, "connections") == 0) {
		test->connection_count = strcmp(value, "n") == 0 ? 2 :
			strtoul(value, NULL, 10);
		return TRUE;
	}
	if (strcmp(key, "messages") == 0) {
		test->message_count = strcmp(value, "all") == 0 ? -1U :
			strtoul(value, NULL, 10);
		return TRUE;
	}
	if (strcmp(key, "state") == 0) {
		if (strcasecmp(value, "nonauth") == 0)
			test->startup_state = TEST_STARTUP_STATE_NONAUTH;
		else if (strcasecmp(value, "auth") == 0)
			test->startup_state = TEST_STARTUP_STATE_DELETED;
		else if (strcasecmp(value, "created") == 0)
			test->startup_state = TEST_STARTUP_STATE_CREATED;
		else if (strcasecmp(value, "appended") == 0)
			test->startup_state = TEST_STARTUP_STATE_APPENDED;
		else if (strcasecmp(value, "selected") == 0)
			test->startup_state = TEST_STARTUP_STATE_SELECTED;
		else {
			*error_r = "Unknown state value";
			return FALSE;
		}
		return TRUE;
	}

	*error_r = "Unknown setting";
	return FALSE;
}

static void
test_parse_imap_arg_dup(pool_t pool, const struct imap_arg *args,
			struct imap_arg *dup)
{
	const struct imap_arg *subargs;
	struct imap_arg *subdub;
	unsigned int i, count;

	dup->type = args->type;
	switch (dup->type) {
	case IMAP_ARG_NIL:
	case IMAP_ARG_EOL:
		break;
	case IMAP_ARG_ATOM:
	case IMAP_ARG_STRING:
	case IMAP_ARG_LITERAL:
		dup->_data.str = p_strdup(pool, args->_data.str);
		break;
	case IMAP_ARG_LIST:
		subargs = array_get(&args->_data.list, &count);
		p_array_init(&dup->_data.list, pool, count);
		for (i = 0; i < count; i++) {
			subdub = array_append_space(&dup->_data.list);
			test_parse_imap_arg_dup(pool, &subargs[i], subdub);
		}
		break;
	default:
		i_unreached();
	}
}

static ARRAY_TYPE(imap_arg_list) *
test_parse_imap_args_dup(pool_t pool, const struct imap_arg *args)
{
	ARRAY_TYPE(imap_arg_list) *list;
	struct imap_arg *dub;
	unsigned int i, count = 0;

	while (args[count++].type != IMAP_ARG_EOL) ;

	list = p_new(pool, ARRAY_TYPE(imap_arg_list), 1);
	p_array_init(list, pool, count);
	for (i = 0; i < count; i++) {
		dub = array_append_space(list);
		test_parse_imap_arg_dup(pool, &args[i], dub);
	}
	return list;
}

ARRAY_TYPE(imap_arg_list) *
test_parse_imap_args(pool_t pool, const char *line, unsigned int linelen,
		     const char **error_r)
{
	struct imap_parser *imap_parser;
	struct istream *input;
	const struct imap_arg *args;
	ARRAY_TYPE(imap_arg_list) *dup_args;
	bool fatal;
	int ret;

	input = i_stream_create_from_data(line, linelen);
	imap_parser = imap_parser_create(input, NULL, (size_t)-1);
	ret = imap_parser_finish_line(imap_parser, 0,
				      IMAP_PARSE_FLAG_LITERAL_TYPE |
				      IMAP_PARSE_FLAG_ATOM_ALLCHARS |
				      IMAP_PARSE_FLAG_MULTILINE_STR, &args);
	if (ret < 0) {
		dup_args = NULL;
		if (ret == -2)
			*error_r = "Missing data";
		else {
			*error_r = t_strdup(imap_parser_get_error(imap_parser,
								  &fatal));
		}
	} else {
		dup_args = test_parse_imap_args_dup(pool, args);
	}
	imap_parser_unref(&imap_parser);
	i_stream_unref(&input);
	return dup_args;
}

struct list_directives_context {
	struct test_parser *parser;
	struct list_directives_context *parent;

	/* the untagged reply name (e.g. FETCH) */
	const char *reply_name;

	/* previous atom before this list, NULL if none */
	const char *prev_atom;
	/* if parent->chain_count > 1: Relative chain index (0..n).
	   (e.g. if we're parsing sublist in FETCH (UID 1 FLAGS (..))
	   we have parent_chain_idx=1 and prev_atom=FLAGS.

	   if parent->chain_count == 1: Chain index is the argument index
	   relative to first one (0..n). */
	unsigned int parent_chain_idx;

	/* Number of elements in a chain (default: 1) */
	unsigned int chain_count;
	/* Directives have been specified for this list */
	bool directives;
};

static bool
list_parse_directives(struct list_directives_context *ctx,
		      const struct imap_arg *args, const char **error_r)
{
	const char *str;

	while (imap_arg_get_atom(args, &str) &&
	       strncmp(str, "$!", 2) == 0) {
		str += 2;

		if (strncmp(str, "unordered", 9) == 0) {
			if (str[9] == '\0')
				;
			else if (str[9] == '=' &&
				 str_to_uint(str + 10, &ctx->chain_count) == 0)
				;
			else {
				*error_r = "Broken $!unordered directive";
				return FALSE;
			}
		} else if (strcmp(str, "ordered") == 0 ||
			   strcmp(str, "noextra") == 0 ||
			   strcmp(str, "extra") == 0 ||
			   strncmp(str, "ignore=", 7) == 0 ||
			   strncmp(str, "ban=", 4) == 0) {
			/* ok */
		} else {
			*error_r = t_strdup_printf("Unknown directive: %s",
						   str);
			return FALSE;
		}
		ctx->directives = TRUE;
		args++;
	}
	return TRUE;
}

static void args_directive(struct list_directives_context *ctx,
			   ARRAY_TYPE(imap_arg_list) *args_arr,
			   const char *directive)
{
	const struct imap_arg *nextarg;
	struct imap_arg *arg;

	nextarg = array_idx(args_arr, 0);
	arg = array_insert_space(args_arr, 0);
	arg->parent = nextarg->parent;
	arg->type = IMAP_ARG_ATOM;
	arg->_data.str = p_strdup(ctx->parser->pool, directive);
}

static void test_add_default_directives(struct list_directives_context *ctx,
					ARRAY_TYPE(imap_arg_list) *args_arr)
{
	if (ctx->parent == NULL)
		return;

	if (strcmp(ctx->reply_name, "list") == 0 ||
	    strcmp(ctx->reply_name, "lsub") == 0) {
		if (ctx->parent->parent == NULL &&
		    ctx->parent->parent_chain_idx == 0 &&
		    ctx->parent_chain_idx == 1) {
			/* list|lsub (flags) sep mailbox */
			args_directive(ctx, args_arr, "$!unordered");
		}
	} else if (strcmp(ctx->reply_name, "status") == 0) {
		if (ctx->parent->parent == NULL &&
		    ctx->parent->parent_chain_idx == 0 &&
		    ctx->parent_chain_idx == 2) {
			/* status <mailbox> (reply) */
			args_directive(ctx, args_arr, "$!unordered=2");
		}
	} else if (strcmp(ctx->reply_name, "fetch") == 0) {
		if (ctx->parent->parent == NULL &&
		    ctx->parent->parent_chain_idx == 0 &&
		    ctx->parent_chain_idx == 2) {
			/* <seq> fetch (reply) */
			args_directive(ctx, args_arr, "$!unordered=2");
		} else if (ctx->parent->parent != NULL &&
			   ctx->parent->parent->parent == NULL &&
			   ctx->parent->parent->parent_chain_idx == 0 &&
			   ctx->parent->parent_chain_idx == 2 &&
			   ctx->prev_atom != NULL &&
			   strcmp(ctx->reply_name, "fetch") == 0 &&
			   strcmp(ctx->prev_atom, "flags") == 0) {
			/* <seq> fetch (flags (..)) */
			args_directive(ctx, args_arr, "$!unordered");
			args_directive(ctx, args_arr, "$!noextra");
			args_directive(ctx, args_arr, "$!ignore=\\recent");
		}
	}
}

static bool
test_parse_untagged_handle_directives(struct list_directives_context *ctx,
				      ARRAY_TYPE(imap_arg_list) *args_arr,
				      const char **error_r)
{
	struct imap_arg *args;
	struct list_directives_context subctx;
	const char *prev_atom = NULL;
	unsigned int i;

	args = array_idx_modifiable(args_arr, 0);

	/* directives exist only at the beginning of a list */
	if (!list_parse_directives(ctx, args, error_r))
		return FALSE;

	if (!ctx->directives) {
		/* no directives specified - see if we could add defaults */
		test_add_default_directives(ctx, args_arr);
		args = array_idx_modifiable(args_arr, 0);
	}

	for (i = 0; args[i].type != IMAP_ARG_EOL; i++) ;
	if (i % ctx->chain_count != 0) {
		*error_r = t_strdup_printf("Invalid list argument count, "
					   "chain size=%u", ctx->chain_count);
		return FALSE;
	}

	for (i = 0; args->type != IMAP_ARG_EOL; args++, i++) {
		if (imap_arg_get_atom(args, &prev_atom))
			prev_atom = t_str_lcase(prev_atom);
		if (args->type != IMAP_ARG_LIST)
			continue;

		memset(&subctx, 0, sizeof(subctx));
		subctx.parser = ctx->parser;
		subctx.parent = ctx;
		subctx.reply_name = t_str_lcase(ctx->reply_name);
		subctx.chain_count = 1;
		subctx.prev_atom = prev_atom;
		subctx.parent_chain_idx = ctx->chain_count == 1 ? i :
			i % ctx->chain_count;

		if (!test_parse_untagged_handle_directives(&subctx,
							   &args->_data.list,
							   error_r))
			return FALSE;
	}
	return TRUE;
}

static bool
test_parse_command_untagged(struct test_parser *parser,
			    const char *line, unsigned int linelen,
			    bool not_found, const char **error_r)
{
	struct test_command *cmd = parser->cur_cmd;
	struct list_directives_context directives_ctx;
	const struct imap_arg *args;
	struct test_untagged ut;
	ARRAY_TYPE(imap_arg_list) *args_arr;
	const char *str = "";

	if (!array_is_created(&cmd->untagged))
		p_array_init(&cmd->untagged, parser->pool, 8);

	args_arr = test_parse_imap_args(parser->pool, line, linelen, error_r);
	if (args_arr == NULL)
		return FALSE;

	args = array_idx(args_arr, 0);
	memset(&directives_ctx, 0, sizeof(directives_ctx));
	directives_ctx.parser = parser;
	directives_ctx.chain_count = 1;
	if (imap_arg_get_atom(args, &str)) {
		if (*str == '$' || (*str >= '0' && *str <= '9')) {
			/* <seq> <reply> */
			if (!imap_arg_get_atom(&args[1], &str))
				str = "";
		}
	}
	directives_ctx.reply_name = str;
	if (!test_parse_untagged_handle_directives(&directives_ctx, args_arr,
						   error_r))
		return FALSE;

	memset(&ut, 0, sizeof(ut));
	ut.args = array_idx(args_arr, 0);
	ut.not_found = not_found;
	array_append(&cmd->untagged, &ut, 1);
	return TRUE;
}

static struct imap_arg *
test_get_cmd_reply(struct test_parser *parser, const char **line)
{
	struct imap_arg *reply = NULL;
	const char *arg = t_strcut(*line, ' ');

	if (strcasecmp(arg, "ok") == 0) {
		reply = parser->reply_ok;
		*line += 2;
	} else if (strcasecmp(arg, "no") == 0) {
		reply = parser->reply_no;
		*line += 2;
	} else if (strcasecmp(arg, "bad") == 0) {
		reply = parser->reply_bad;
		*line += 3;
	} else if (strcasecmp(arg, "\"\"") == 0) {
		reply = parser->reply_any;
		*line += 2;
	}
	if (reply != NULL && **line == ' ')
		*line += 1;
	return reply;
}

static bool
test_parse_command_finish(struct test_parser *parser,
			  const char *line, unsigned int linelen,
			  const char **error_r)
{
	struct test_command *cmd = parser->cur_cmd;
	ARRAY_TYPE(imap_arg_list) *args;

	args = test_parse_imap_args(parser->pool, line, linelen, error_r);
	cmd->reply = array_idx(args, 0);
	return cmd->reply != NULL;
}

static bool
test_parse_command_line(struct test_parser *parser, struct test *test,
			unsigned int linenum, const char *line,
			unsigned int linelen, const char **error_r)
{
	struct test_command *cmd;
	const char *line2, *p;
	void *cmdmem;

	if (parser->cur_cmd != NULL) {
		if (strncmp(line, "* ", 2) == 0 ||
		    strncmp(line, "! ", 2) == 0) {
			bool not_found = line[0] == '!';
			return test_parse_command_untagged(parser, line + 2,
							   linelen-2, not_found,
							   error_r);
		}
		line2 = line;
		if (parser->cur_cmd->reply == NULL &&
		    test_get_cmd_reply(parser, &line2) != NULL) {
			return test_parse_command_finish(parser, line, linelen,
							 error_r);
		}
	}

	if (parser->cur_cmd != NULL && parser->cur_cmd->reply == NULL) {
		*error_r = "Missing reply from previous command";
		return FALSE;
	}

	cmd = p_new(parser->pool, struct test_command, 1);
	cmd->linenum = linenum;
	if (test->connection_count > 1) {
		/* begins with connection index */
		if (str_to_uint(t_strcut(line, ' '), &cmd->connection_idx) < 0 ||
		    cmd->connection_idx == 0) {
			*error_r = "Missing client index";
			return FALSE;
		}

		i_assert(cmd->connection_idx > 0);
		if (test->connection_count < cmd->connection_idx)
			test->connection_count = cmd->connection_idx;
		cmd->connection_idx--;

		p = strchr(line, ' ');
		if (p == NULL) {
			line = "";
			linelen = 0;
		} else {
			linelen -= p+1 - line;
			line = p+1;
		}
	}

	/* optional expected ok/no/bad reply */
	p = line;
	cmd->reply = test_get_cmd_reply(parser, &p);
	linelen -= p-line;
	line = p;

	i_assert(line[linelen] == '\0');

	cmd->command = cmdmem = p_malloc(parser->pool, linelen+1);
	memcpy(cmdmem, line, linelen);
	cmd->command_len = linelen;
	parser->cur_cmd = cmd;
	array_append(&test->commands, &cmd, 1);
	return TRUE;
}

static bool test_parse_file(struct test_parser *parser, struct test *test,
			    struct istream *input)
{
	const char *error;
	string_t *line, *multiline;
	const unsigned char *data, *p;
	size_t size;
	unsigned int len, linenum = 0, start_linenum = 0, start_pos = 0, last_line_end = 0;
	int ret;
	bool ok, header = TRUE, continues = FALSE, binary = FALSE;

	line = t_str_new(256);
	multiline = t_str_new(256);
	parser->cur_cmd = NULL;
	while ((ret = i_stream_read_data(input, &data, &size, 0)) > 0) {
		p = memchr(data, '\n', size);
		if (p == NULL)
			break;
		str_truncate(line, 0);
		buffer_append(line, data, p-data);
		i_stream_skip(input, p-data+1);

		linenum++;
		if (continues) {
			if (strncmp(str_c(line), "}}}", 3) != 0) {
				str_append_str(multiline, line);
				last_line_end = str_len(multiline);
				if (binary && !i_stream_last_line_crlf(input))
					str_append(multiline, "\n");
				else
					str_append(multiline, "\r\n");
				continue;
			}
			str_truncate(multiline, last_line_end);
			str_delete(line, 0, 3);
			str_insert(multiline, start_pos, t_strdup_printf(
				"{%"PRIuSIZE_T"}\r\n", str_len(multiline)-start_pos));

			len = str_len(line);
			if (len >= 3 && strcmp(str_c(line) + len-3, "{{{") == 0) {
				if (len > 3 && str_c(line)[len-4] == '~') {
					len--;
					binary = TRUE;
				} else {
					binary = FALSE;
				}
				buffer_append(multiline, str_data(line), len-3);
				start_pos = str_len(multiline);
				last_line_end = str_len(multiline);
				continue;
			}
			str_append_str(multiline, line);
			str_truncate(line, 0);
			str_append_str(line, multiline);
			continues = FALSE;
		} else {
			start_linenum = linenum;
			if (str_len(line) == 0) {
				header = FALSE;
				continue;
			}
			if (*str_c(line) == '#')
				continue;

			len = str_len(line);
			if (len >= 3 && strcmp(str_c(line) + len-3, "{{{") == 0) {
				str_truncate(multiline, 0);
				if (len > 3 && str_c(line)[len-4] == '~') {
					len--;
					binary = TRUE;
				} else {
					binary = FALSE;
				}
				buffer_append(multiline, str_data(line), len-3);
				start_pos = str_len(multiline);
				last_line_end = str_len(multiline);
				continues = TRUE;
				continue;
			}
		}

		T_BEGIN {
			if (header) {
				ok = test_parse_header_line(parser, test,
							    str_c(line), &error);
			} else {
				ok = test_parse_command_line(parser, test,
							     start_linenum,
							     str_c(line),
							     line->used, &error);
			}
			if (!ok) {
				i_error("%s line %u: %s", test->path,
					linenum, error);
			}
		} T_END;

		if (!ok)
			return FALSE;
	}
	if (continues) {
		i_error("%s: Multiline reply at line %u not ended",
			test->path, start_linenum);
		return FALSE;
	}
	if (parser->cur_cmd == NULL) {
		i_error("%s: No commands in file", test->path);
		return FALSE;
	}
	if (parser->cur_cmd != NULL && parser->cur_cmd->reply == NULL) {
		i_error("%s line %u: Missing reply from previous command at line %u",
			test->path, linenum, parser->cur_cmd->linenum);
		return FALSE;
	}
	return TRUE;
}

static void test_add_logout(struct test_parser *parser, struct test *test,
			    unsigned int connection_idx)
{
	struct test_command *cmd;

	cmd = p_new(parser->pool, struct test_command, 1);
	cmd->connection_idx = connection_idx;
	cmd->reply = parser->reply_ok;
	cmd->command = "logout";
	cmd->command_len = strlen(cmd->command);
	array_append(&test->commands, &cmd, 1);
}

static int
test_parser_read_test(struct test_parser *parser, const char *fname,
		      const struct test **test_r)
{
	struct test *test;
	struct istream *input;
	struct stat st;
	const char *mbox_path;
	int fd, ret = 0;

	test = p_new(parser->pool, struct test, 1);
	test->startup_state = TEST_STARTUP_STATE_SELECTED;
	test->connection_count = 1;
	test->message_count = -1U;
	p_array_init(&test->commands, parser->pool, 32);

	mbox_path = t_strdup_printf("%s/%s.mbox", parser->dir, fname);
	if (stat(mbox_path, &st) == 0) {
		/* test-specific mbox */
		test->mbox_source_path = p_strdup(parser->pool, mbox_path);
	} else if (errno != ENOENT) {
		i_error("stat(%s) failed: %m", mbox_path);
		return -1;
	} else {
		/* use the default mbox */
		test->mbox_source_path = parser->default_mbox_path;
	}

	test->path = p_strdup_printf(parser->pool, "%s/%s", parser->dir, fname);
	test->name = test->path + strlen(parser->dir) + 1;
	if (stat(test->path, &st) < 0) {
		i_error("stat(%s) failed: %m", test->path);
		return -1;
	}
	if (!S_ISREG(st.st_mode))
		return 0;

	fd = open(test->path, O_RDONLY);
	if (fd == -1) {
		i_error("open(%s) failed: %m", test->path);
		return -1;
	}

	input = i_stream_create_fd(fd, (size_t)-1, FALSE);
	if (!test_parse_file(parser, test, input))
		ret = -1;
	i_stream_unref(&input);
	if (close(fd) < 0) {
		i_error("close(%s) failed: %m", test->path);
		return -1;
	}
	/* add logout to only one connection, since we can't handle
	   disconnections before all the connections have received tagged
	   OK for logout */
	test_add_logout(parser, test, 0);

	*test_r = test;
	return ret < 0 ? -1 : 1;
}

static int test_parser_scan_dir(struct test_parser *parser)
{
	const struct test *test;
	DIR *dir;
	struct dirent *d;
	unsigned int len;
	int ret = 0;

	dir = opendir(parser->dir);
	if (dir == NULL) {
		i_error("opendir(%s) failed: %m", parser->dir);
		return -1;
	}

	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.')
			continue;
		len = strlen(d->d_name);
		if (len >= 5 && strcmp(d->d_name + len - 5, ".mbox") == 0)
			continue;

		T_BEGIN {
			ret = test_parser_read_test(parser, d->d_name, &test);
		} T_END;
		if (ret < 0)
			break;
		if (ret > 0)
			array_append(&parser->tests, &test, 1);
	}
	if (closedir(dir) < 0) {
		i_error("closedir(%s) failed: %m", parser->dir);
		return -1;
	}
	return ret;
}

static struct imap_arg *test_parser_reply_init(pool_t pool, const char *atom)
{
	struct imap_arg *args;

	args = p_new(pool, struct imap_arg, 2);
	args[0].type = IMAP_ARG_ATOM;
	args[0]._data.str = p_strdup(pool, atom);
	args[1].type = IMAP_ARG_EOL;
	return args;
}

struct test_parser *test_parser_init(const char *dir)
{
	struct test_parser *parser;
	pool_t pool;

	pool = pool_alloconly_create("test parser", 1024*256);
	parser = p_new(pool, struct test_parser, 1);
	parser->pool = pool;
	parser->dir = p_strdup(pool, dir);
	parser->default_mbox_path =
		p_strdup_printf(pool, "%s/"DEFAULT_MBOX_FNAME, dir);
	i_array_init(&parser->tests, 128);

	parser->reply_ok = test_parser_reply_init(pool, "ok");
	parser->reply_no = test_parser_reply_init(pool, "no");
	parser->reply_bad = test_parser_reply_init(pool, "bad");
	parser->reply_any = test_parser_reply_init(pool, "");

	if (test_parser_scan_dir(parser) < 0)
		i_fatal("Failed to read tests");
	return parser;
}

void test_parser_deinit(struct test_parser **_parser)
{
	struct test_parser *parser = *_parser;

	*_parser = NULL;
	array_free(&parser->tests);
	pool_unref(&parser->pool);
}

const ARRAY_TYPE(test) *test_parser_get_tests(struct test_parser *parser)
{
	return &parser->tests;
}
