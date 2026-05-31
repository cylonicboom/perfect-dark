/**
 * Action block (ailist) -> Lua transpiler.
 *
 * This file deliberately has NO dependency on the game engine so that it can be
 * compiled both into the game (used by luaai.c) and into standalone tooling and
 * unit tests (tools/luaai_test). It only uses the C standard library.
 *
 * Output shape (for a list with commands at offsets 0, 3, 8, ...):
 *
 *   return function(ctx)
 *     while true do
 *       local off = ctx:cur()
 *       if false then
 *       elseif off == 0 then goto L_0
 *       elseif off == 3 then goto L_3
 *       else return 0 end
 *       ::L_0:: do local r = ctx:exec(0) if r ~= 0 then return r end end goto NEXT
 *       ::L_3:: do local r = ctx:exec(3) if r ~= 0 then return r end end goto NEXT
 *       ::NEXT::
 *     end
 *   end
 *
 * ctx:cur()      returns the current command offset (the program counter).
 * ctx:exec(off)  runs the original C handler for the command at off and returns:
 *                  0 = continue to the next command (advance / jump handled in C)
 *                  1 = yield this frame (stop running this list until next frame)
 *                  2 = the active list changed or terminated (driver takes over)
 *
 * Because ctx:exec mutates the program counter exactly like the original
 * interpreter (including label jumps via chraiGoToLabel), the Lua dispatch loop
 * reproduces the original control flow precisely while running through Lua.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-contained prototype (mirrors the one in game/luaai.h) so that this
 * translation unit has no dependency on any game engine headers and can be
 * compiled into standalone tooling and tests. */
char *luaaiTranspile(const unsigned char *list, unsigned int maxlen,
		unsigned int (*cmdlen)(const unsigned char *list, unsigned int off),
		unsigned int endopcode);

/* A small growable string buffer. */
struct strbuf {
	char *data;
	unsigned int len;
	unsigned int cap;
	int oom;
};

static void sb_init(struct strbuf *sb)
{
	sb->cap = 1024;
	sb->len = 0;
	sb->oom = 0;
	sb->data = (char *)malloc(sb->cap);
	if (!sb->data) {
		sb->oom = 1;
		sb->cap = 0;
	} else {
		sb->data[0] = '\0';
	}
}

static void sb_ensure(struct strbuf *sb, unsigned int extra)
{
	unsigned int need;

	if (sb->oom) {
		return;
	}

	need = sb->len + extra + 1;
	if (need <= sb->cap) {
		return;
	}

	while (sb->cap < need) {
		sb->cap *= 2;
	}

	{
		char *tmp = (char *)realloc(sb->data, sb->cap);
		if (!tmp) {
			sb->oom = 1;
			return;
		}
		sb->data = tmp;
	}
}

static void sb_puts(struct strbuf *sb, const char *s)
{
	unsigned int n;

	if (sb->oom) {
		return;
	}

	n = (unsigned int)strlen(s);
	sb_ensure(sb, n);
	if (sb->oom) {
		return;
	}

	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
}

static void sb_putu(struct strbuf *sb, unsigned int v)
{
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%u", v);
	sb_puts(sb, tmp);
}

static void sb_puthex(struct strbuf *sb, unsigned int v)
{
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "0x%04x", v);
	sb_puts(sb, tmp);
}

char *luaaiTranspile(const unsigned char *list, unsigned int maxlen,
		unsigned int (*cmdlen)(const unsigned char *list, unsigned int off),
		unsigned int endopcode)
{
	struct strbuf sb;
	unsigned int *offsets;
	unsigned int *opcodes;
	unsigned int count;
	unsigned int cap;
	unsigned int off;
	unsigned int i;

	if (!list || !cmdlen) {
		return NULL;
	}

	/* First pass: walk the list and collect command start offsets, stopping
	 * at the end opcode (which is not itself emitted as a dispatchable block,
	 * so reaching it falls through to "return 0"). */
	cap = 64;
	count = 0;
	offsets = (unsigned int *)malloc(cap * sizeof(unsigned int));
	opcodes = (unsigned int *)malloc(cap * sizeof(unsigned int));
	if (!offsets || !opcodes) {
		free(offsets);
		free(opcodes);
		return NULL;
	}

	off = 0;
	while (off + 1 < maxlen) {
		unsigned int op = ((unsigned int)list[off] << 8) | (unsigned int)list[off + 1];
		unsigned int len;

		if (op == endopcode) {
			break;
		}

		if (count >= cap) {
			unsigned int *t1;
			unsigned int *t2;
			cap *= 2;
			t1 = (unsigned int *)realloc(offsets, cap * sizeof(unsigned int));
			t2 = (unsigned int *)realloc(opcodes, cap * sizeof(unsigned int));
			if (!t1 || !t2) {
				free(t1 ? t1 : offsets);
				free(t2 ? t2 : opcodes);
				return NULL;
			}
			offsets = t1;
			opcodes = t2;
		}

		offsets[count] = off;
		opcodes[count] = op;
		count++;

		len = cmdlen(list, off);
		if (len == 0) {
			/* Defensive: avoid an infinite loop on a malformed list. */
			break;
		}
		off += len;
	}

	/* Second pass: emit the Lua chunk. */
	sb_init(&sb);

	sb_puts(&sb, "return function(ctx)\n");
	sb_puts(&sb, "  while true do\n");
	sb_puts(&sb, "    local off = ctx:cur()\n");
	sb_puts(&sb, "    if false then\n");

	for (i = 0; i < count; i++) {
		sb_puts(&sb, "    elseif off == ");
		sb_putu(&sb, offsets[i]);
		sb_puts(&sb, " then goto L_");
		sb_putu(&sb, offsets[i]);
		sb_puts(&sb, "\n");
	}

	sb_puts(&sb, "    else return 0 end\n");

	for (i = 0; i < count; i++) {
		sb_puts(&sb, "    ::L_");
		sb_putu(&sb, offsets[i]);
		sb_puts(&sb, ":: do local r = ctx:exec(");
		sb_putu(&sb, offsets[i]);
		sb_puts(&sb, ") if r ~= 0 then return r end end goto NEXT -- ");
		sb_puthex(&sb, opcodes[i]);
		sb_puts(&sb, "\n");
	}

	sb_puts(&sb, "    ::NEXT::\n");
	sb_puts(&sb, "  end\n");
	sb_puts(&sb, "end\n");

	free(offsets);
	free(opcodes);

	if (sb.oom) {
		free(sb.data);
		return NULL;
	}

	return sb.data;
}
