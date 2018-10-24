//
// sdn: simple directory navigator
//
// Copyright (c) 2017 - 2018, Přemysl Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

// May be required for ncursesw and we generally want it all anyway
#define _XOPEN_SOURCE_EXTENDED

#include <string>
#include <vector>
#include <locale>
#include <iostream>
#include <algorithm>
#include <cwchar>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <map>

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/acl.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#include <sys/inotify.h>
#include <sys/xattr.h>
#include <acl/libacl.h>
#include <ncurses.h>

// Unicode is complex enough already and we might make assumptions
#ifndef __STDC_ISO_10646__
#error Unicode required for wchar_t
#endif

// Trailing return types make C++ syntax suck considerably less
#define fun static auto

#ifndef A_ITALIC
#define A_ITALIC 0
#endif

using namespace std;

// For some reason handling of encoding in C and C++ is extremely annoying
// and C++17 ironically obsoletes C++11 additions that made it less painful
fun to_wide (const string &multi) -> wstring {
	wstring wide; wchar_t w; mbstate_t mb {};
	size_t n = 0, len = multi.length () + 1;
	while (auto res = mbrtowc (&w, multi.c_str () + n, len - n, &mb)) {
		if (res == size_t (-1) || res == size_t (-2))
			return L"/invalid encoding/";

		n += res;
		wide += w;
	}
	return wide;
}

fun to_mb (const wstring &wide) -> string {
	string mb; char buf[MB_LEN_MAX + 1]; mbstate_t mbs {};
	for (size_t n = 0; n <= wide.length (); n++) {
		auto res = wcrtomb (buf, wide.c_str ()[n], &mbs);
		if (res == size_t (-1))
			throw invalid_argument ("invalid encoding");
		mb.append (buf, res);
	}
	// There's one extra NUL character added by wcrtomb()
	mb.erase (mb.length () - 1);
	return mb;
}

fun prefix_length (const wstring &in, const wstring &of) -> int {
	int score = 0;
	for (size_t i = 0; i < of.size () && in.size () >= i && in[i] == of[i]; i++)
		score++;
	return score;
}

fun split (const string &s, const string &sep, vector<string> &out) {
	size_t mark = 0, p = s.find (sep);
	for (; p != string::npos; p = s.find (sep, (mark = p + sep.length ())))
		if (mark < p)
			out.push_back (s.substr (mark, p - mark));
	if (mark < s.length ())
		out.push_back (s.substr (mark));
}

fun split (const string &s, const string &sep) -> vector<string> {
	vector<string> result; split (s, sep, result); return result;
}

fun shell_escape (const string &v) -> string {
	string result;
	for (auto c : v)
		if (c == '\'')
			result += "'\\''";
		else
			result += c;
	return "'" + result + "'";
}

fun decode_type (mode_t m) -> wchar_t {
	if (S_ISDIR  (m)) return L'd'; if (S_ISBLK  (m)) return L'b';
	if (S_ISCHR  (m)) return L'c'; if (S_ISLNK  (m)) return L'l';
	if (S_ISFIFO (m)) return L'p'; if (S_ISSOCK (m)) return L's';
	return L'-';
}

/// Return the modes of a file in the usual stat/ls format
fun decode_mode (mode_t m) -> wstring {
	return { decode_type (m),
		L"r-"[!(m & S_IRUSR)],
		L"w-"[!(m & S_IWUSR)],
		((m & S_ISUID) ? L"sS" : L"x-")[!(m & S_IXUSR)],
		L"r-"[!(m & S_IRGRP)],
		L"w-"[!(m & S_IWGRP)],
		((m & S_ISGID) ? L"sS" : L"x-")[!(m & S_IXGRP)],
		L"r-"[!(m & S_IROTH)],
		L"w-"[!(m & S_IWOTH)],
		((m & S_ISVTX) ? L"tT" : L"x-")[!(m & S_IXOTH)],
	};
}

template<class T> fun shift (vector<T> &v) -> T {
	auto front = v.front (); v.erase (begin (v)); return front;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

fun xdg_config_home () -> string {
	const char *user_dir = getenv ("XDG_CONFIG_HOME");
	if (user_dir && user_dir[0] == '/')
		return user_dir;

	const char *home_dir = getenv ("HOME");
	return string (home_dir ? home_dir : "") + "/.config";
}

// In C++17 we will get <optional> but until then there's unique_ptr
fun xdg_config_find (const string &suffix) -> unique_ptr<ifstream> {
	vector<string> dirs {xdg_config_home ()};
	const char *system_dirs = getenv ("XDG_CONFIG_DIRS");
	split (system_dirs ? system_dirs : "/etc/xdg", ":", dirs);
	for (const auto &dir : dirs) {
		if (dir[0] != '/')
			continue;
		if (ifstream ifs {dir + suffix})
			return make_unique<ifstream> (move (ifs));
	}
	return nullptr;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

using ncstring = basic_string<cchar_t>;

fun cchar (chtype attrs, wchar_t c) -> cchar_t {
	cchar_t ch {};
	setcchar (&ch, &c, attrs, PAIR_NUMBER (attrs), nullptr);
	return ch;
}

fun decolor (cchar_t &ch) {
	wchar_t c[CCHARW_MAX]; attr_t attrs; short pair;
	getcchar (&ch, c, &attrs, &pair, nullptr);
	setcchar (&ch, c, attrs &~ A_REVERSE, 0, nullptr);
}

fun apply_attrs (const wstring &w, attr_t attrs) -> ncstring {
	ncstring res;
	for (auto c : w)
		res += cchar (attrs, c);
	return res;
}

fun sanitize_char (chtype attrs, wchar_t c) -> ncstring {
	if (c < 32)
		return {cchar (attrs | A_REVERSE, L'^'),
				cchar (attrs | A_REVERSE, c + 64)};
	if (!iswprint (c))
		return {cchar (attrs | A_REVERSE, L'?')};
	return {cchar (attrs, c)};
}

fun sanitize (const ncstring &nc) -> ncstring {
	ncstring out;
	for (cchar_t c : nc)
		for (size_t i = 0; i < CCHARW_MAX && c.chars[i]; i++)
			out += sanitize_char (c.attr, c.chars[i]);
	return out;
}

fun print (const ncstring &nc, int limit) -> int {
	int total_width = 0;
	for (cchar_t c : sanitize (nc)) {
		int width = wcwidth (c.chars[0]);
		if (total_width + width > limit)
			break;

		add_wch (&c);
		total_width += width;
	}
	return total_width;
}

fun compute_width (const ncstring &nc) -> int {
	int total = 0;
	for (const auto &c : nc)
		total += wcwidth (c.chars[0]);
	return total;
}

// TODO: maybe we need formatting for the padding passed in?
fun align (const ncstring &nc, int target) -> ncstring {
	auto current = compute_width (nc);
	auto missing = abs (target) - current;
	if (missing <= 0)
		return nc;
	return target < 0
		? nc + apply_attrs (wstring (missing, L' '), 0)
		: apply_attrs (wstring (missing, L' '), 0) + nc;
}

fun allocate_pair (short fg, short bg) -> short {
	static short counter = 1; init_pair (counter, fg, bg); return counter++;
}

fun decode_attrs (const vector<string> &attrs) -> chtype {
	chtype result = 0; int fg = -1, bg = -1, colors = 0;
	for (const auto &s : attrs) {
		char *end; auto color = strtol (s.c_str (), &end, 10);
		if (!*end && color >= -1 && color < COLORS) {
			if    (++colors == 1) fg = color;
			else if (colors == 2) bg = color;
		}
		else if (s == "bold")    result |= A_BOLD;
		else if (s == "dim")     result |= A_DIM;
		else if (s == "ul")      result |= A_UNDERLINE;
		else if (s == "blink")   result |= A_BLINK;
		else if (s == "reverse") result |= A_REVERSE;
		else if (s == "italic")  result |= A_ITALIC;
	}
	if (fg != -1 || bg != -1)
		result |= COLOR_PAIR (allocate_pair (fg, bg));
	return result;
}

// --- Application -------------------------------------------------------------

#define CTRL 31 &

struct row {
	enum { MODES, USER, GROUP, SIZE, MTIME, FILENAME, COLUMNS };
	ncstring cols[COLUMNS];
};

struct entry {
	// TODO: how to present symlink target, stat of the target?
	//   unique_ptr<string> target; struct stat target_info;
	string filename; struct stat info; struct row row;
	auto operator< (const entry &other) -> bool {
		auto a = S_ISDIR (info.st_mode);
		auto b = S_ISDIR (other.info.st_mode);
		return (a && !b) || (a == b && filename < other.filename);
	}
};

#define LS(XX) XX(NORMAL, "no") XX(FILE, "fi") XX(RESET, "rs") \
	XX(DIRECTORY, "di") XX(SYMLINK, "ln") XX(MULTIHARDLINK, "mh") \
	XX(FIFO, "pi") XX(SOCKET, "so") XX(DOOR, "do") XX(BLOCK, "bd") \
	XX(CHARACTER, "cd") XX(ORPHAN, "or") XX(MISSING, "mi") XX(SETUID, "su") \
	XX(SETGID, "sg") XX(CAPABILITY, "ca") XX(STICKY_OTHER_WRITABLE, "tw") \
	XX(OTHER_WRITABLE, "ow") XX(STICKY, "st") XX(EXECUTABLE, "ex")

#define XX(id, name) LS_ ## id,
enum { LS(XX) LS_COUNT };
#undef XX

#define XX(id, name) name,
static const char *g_ls_colors[] = {LS(XX)};
#undef XX

static struct {
	string cwd;                         ///< Current working directory
	string start_dir;                   ///< Starting directory
	vector<entry> entries;              ///< Current directory entries
	int offset, cursor;                 ///< Scroll offset and cursor position
	bool full_view;                     ///< Show extended information
	int max_widths[row::COLUMNS];       ///< Column widths

	string chosen;                      ///< Chosen item for the command line
	bool chosen_full;                   ///< Use the full path

	int inotify_fd, inotify_wd = -1;    ///< File watch
	bool out_of_date;                   ///< Entries may be out of date

	wchar_t editor;                     ///< Prompt character for editing
	wstring editor_line;                ///< Current user input

	enum { AT_CURSOR, AT_BAR, AT_CWD, AT_INPUT, AT_COUNT };
	chtype attrs[AT_COUNT] = {A_REVERSE, 0, A_BOLD, 0};
	const char *attr_names[AT_COUNT] = {"cursor", "bar", "cwd", "input"};

	map<int, chtype> ls_colors;         ///< LS_COLORS decoded
	map<string, chtype> ls_exts;        ///< LS_COLORS file extensions

	// Refreshed by reload():

	map<uid_t, string> unames;          ///< User names by UID
	map<gid_t, string> gnames;          ///< Group names by GID
	struct tm now;                      ///< Current local time for display
} g;

fun ls_format (const string &filename, const struct stat &info) -> chtype {
	int type = LS_ORPHAN;
	auto set = [&](int t) { if (g.ls_colors.count (t)) type = t; };
	// TODO: LS_MISSING if available and this is a missing symlink target
	// TODO: go by readdir() information when stat() isn't available yet
	if (S_ISREG (info.st_mode)) {
		type = LS_FILE;
		if (info.st_nlink > 1)
			set (LS_MULTIHARDLINK);
		if ((info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
			set (LS_EXECUTABLE);
		if (lgetxattr (filename.c_str (), "security.capability", NULL, 0) >= 0)
			set (LS_CAPABILITY);
		if ((info.st_mode & S_ISGID))
			set (LS_SETGID);
		if ((info.st_mode & S_ISUID))
			set (LS_SETUID);
	} else if (S_ISDIR (info.st_mode)) {
		type = LS_DIRECTORY;
		if ((info.st_mode & S_ISVTX))
			set (LS_STICKY);
		if ((info.st_mode & S_IWOTH))
			set (LS_OTHER_WRITABLE);
		if ((info.st_mode & S_ISVTX) && (info.st_mode & S_IWOTH))
			set (LS_STICKY_OTHER_WRITABLE);
	} else if (S_ISLNK (info.st_mode)) {
		// TODO: LS_ORPHAN when symlink target is missing and either
		//   a/ "li" is "target", or
		//   b/ LS_ORPHAN is available
		type = LS_SYMLINK;
	} else if (S_ISFIFO (info.st_mode)) {
		type = LS_FIFO;
	} else if (S_ISSOCK (info.st_mode)) {
		type = LS_SOCKET;
	} else if (S_ISBLK (info.st_mode)) {
		type = LS_BLOCK;
	} else if (S_ISCHR (info.st_mode)) {
		type = LS_CHARACTER;
	}

	chtype format = 0;
	const auto x = g.ls_colors.find (type);
	if (x != g.ls_colors.end ())
		format = x->second;

	auto dot = filename.find_last_of ('.');
	if (dot != string::npos && type == LS_FILE) {
		const auto x = g.ls_exts.find (filename.substr (++dot));
		if (x != g.ls_exts.end ())
			format = x->second;
	}
	return format;
}

// XXX: this will probably have to be changed to make_entry and run lstat itself
fun make_row (const string &filename, const struct stat &info) -> row {
	row r;
	auto mode = decode_mode (info.st_mode);
	// This is a Linux-only extension
	if (acl_extended_file_nofollow (filename.c_str ()) > 0)
		mode += L"+";
	r.cols[row::MODES] = apply_attrs (mode, 0);

	auto usr = g.unames.find (info.st_uid);
	r.cols[row::USER] = (usr != g.unames.end ())
		? apply_attrs (to_wide (usr->second), 0)
		: apply_attrs (to_wstring (info.st_uid), 0);

	auto grp = g.gnames.find (info.st_gid);
	r.cols[row::GROUP] = (grp != g.unames.end ())
		? apply_attrs (to_wide (grp->second), 0)
		: apply_attrs (to_wstring (info.st_gid), 0);

	auto size = to_wstring (info.st_size);
	if      (info.st_size >> 40) size = to_wstring (info.st_size >> 40) + L"T";
	else if (info.st_size >> 30) size = to_wstring (info.st_size >> 30) + L"G";
	else if (info.st_size >> 20) size = to_wstring (info.st_size >> 20) + L"M";
	else if (info.st_size >> 10) size = to_wstring (info.st_size >> 10) + L"K";
	r.cols[row::SIZE] = apply_attrs (size, 0);

	char buf[32] = "";
	auto tm = localtime (&info.st_mtime);
	strftime (buf, sizeof buf,
		(tm->tm_year == g.now.tm_year) ? "%b %e %H:%M" : "%b %e  %Y", tm);
	r.cols[row::MTIME] = apply_attrs (to_wide (buf), 0);

	// TODO: show symlink target: check st_mode/DT_*, readlink
	auto format = ls_format (filename, info);
	r.cols[row::FILENAME] = apply_attrs (to_wide (filename), format);
	return r;
}

fun inline visible_lines () -> int { return max (0, LINES - 2); }

fun update () {
	int start_column = g.full_view ? 0 : row::FILENAME;
	static int alignment[row::COLUMNS] = { -1, -1, -1, 1, -1, -1 };
	erase ();

	int available = visible_lines ();
	int used = min (available, int (g.entries.size ()) - g.offset);
	for (int i = 0; i < used; i++) {
		auto index = g.offset + i;
		bool selected = index == g.cursor;
		attrset (selected ? g.attrs[g.AT_CURSOR] : 0);
		move (available - used + i, 0);

		auto used = 0;
		for (int col = start_column; col < row::COLUMNS; col++) {
			const auto &field = g.entries[index].row.cols[col];
			auto aligned = align (field, alignment[col] * g.max_widths[col]);
			if (selected)
				for_each (begin (aligned), end (aligned), decolor);
			used += print (aligned + apply_attrs (L" ", 0), COLS - used);
		}
		hline (' ', COLS - used);
	}

	auto bar = apply_attrs (to_wide (g.cwd), g.attrs[g.AT_CWD]);
	if (g.out_of_date)
		bar += apply_attrs (L" [+]", 0);

	move (LINES - 2, 0);
	attrset (g.attrs[g.AT_BAR]);
	hline (' ', COLS - print (bar, COLS));

	attrset (g.attrs[g.AT_INPUT]);
	if (g.editor) {
		move (LINES - 1, 0);
		auto p = apply_attrs ({g.editor, L' ', L'\0'}, 0);
		move (LINES - 1, print (p + apply_attrs (g.editor_line, 0), COLS - 1));
		curs_set (1);
	} else
		curs_set (0);

	refresh ();
}

fun reload () {
	g.unames.clear();
	while (auto *ent = getpwent ()) g.unames.emplace(ent->pw_uid, ent->pw_name);
	endpwent();

	g.gnames.clear();
	while (auto *ent = getgrent ()) g.gnames.emplace(ent->gr_gid, ent->gr_name);
	endgrent();

	auto now = time (NULL); g.now = *localtime (&now);
	char buf[4096]; g.cwd = getcwd (buf, sizeof buf);

	auto dir = opendir (".");
	g.entries.clear ();
	while (auto f = readdir (dir)) {
		// Two dots are for navigation but this ain't as useful
		if (f->d_name == string ("."))
			continue;

		// TODO: check lstat() return value
		// TODO: benchmark just readdir() vs. lstat(), also on dead mounts;
		//   it might make sense to stat asynchronously in threads
		//   http://lkml.iu.edu/hypermail//linux/kernel/0804.3/1616.html
		struct stat sb = {};
		lstat (f->d_name, &sb);

		auto row = make_row (f->d_name, sb);
		if (S_ISLNK (sb.st_mode)) {
			char buf[PATH_MAX] = {};
			auto len = readlink (f->d_name, buf, sizeof buf);
			if (len < 0 || size_t (len) >= sizeof buf) {
				buf[0] = '?';
				buf[1] = 0;
			}

			struct stat sbt = {};
			lstat (buf, &sbt);

			row.cols[row::FILENAME].append (apply_attrs (to_wide (" -> "), 0))
				.append (apply_attrs (to_wide (buf), ls_format (buf, sbt)));
		}
		g.entries.push_back ({ f->d_name, sb, row });
	}
	closedir (dir);
	sort (begin (g.entries), end (g.entries));
	g.out_of_date = false;

	for (int col = 0; col < row::COLUMNS; col++) {
		auto &longest = g.max_widths[col] = 0;
		for (const auto &entry : g.entries)
			longest = max (longest, compute_width (entry.row.cols[col]));
	}

	g.cursor = min (g.cursor, int (g.entries.size ()) - 1);
	g.offset = min (g.offset, int (g.entries.size ()) - 1);

	if (g.inotify_wd != -1)
		inotify_rm_watch (g.inotify_fd, g.inotify_wd);

	// We don't show atime, so access and open are merely spam
	g.inotify_wd = inotify_add_watch (g.inotify_fd, buf,
		(IN_ALL_EVENTS | IN_ONLYDIR | IN_EXCL_UNLINK) & ~(IN_ACCESS | IN_OPEN));
}

fun search (const wstring &needle) {
	int best = g.cursor, best_n = 0;
	for (int i = 0; i < int (g.entries.size ()); i++) {
		auto o = (i + g.cursor) % g.entries.size ();
		int n = prefix_length (to_wide (g.entries[o].filename), needle);
		if (n > best_n) {
			best = o;
			best_n = n;
		}
	}
	g.cursor = best;
}

fun handle_editor (wint_t c, bool is_char) {
	if (c == 27 || c == (CTRL L'g')) {
		g.editor_line.clear ();
		g.editor = 0;
	} else if (c == L'\r' || (!is_char && c == KEY_ENTER)) {
		if (g.editor == L'e') {
			auto mb = to_mb (g.editor_line);
			rename (g.entries[g.cursor].filename.c_str (), mb.c_str ());
			reload ();
		}
		g.editor_line.clear ();
		g.editor = 0;
	} else if (is_char) {
		g.editor_line += c;
		if (g.editor == L'/'
		 || g.editor == L's')
			search (g.editor_line);
	} else if (c == KEY_BACKSPACE) {
		if (!g.editor_line.empty ())
			g.editor_line.erase (g.editor_line.length () - 1);
	} else
		beep ();
}

fun change_dir (const string& path) {
	if (chdir (path.c_str ())) {
		beep ();
	} else {
		// TODO: remember cursor going down, then restore going up
		g.cursor = 0;
		reload ();
	}
}

fun choose (const entry &entry) -> bool {
	bool is_dir = S_ISDIR (entry.info.st_mode) != 0;
	// Dive into directories and accessible symlinks to them
	// TODO: we probably want to use a preread readlink value
	if (S_ISLNK (entry.info.st_mode)) {
		char buf[PATH_MAX];
		struct stat sb = {};
		auto len = readlink (entry.filename.c_str (), buf, sizeof buf);
		is_dir = len > 0 && size_t (len) < sizeof buf
			&& !stat (entry.filename.c_str (), &sb)
			&& S_ISDIR (sb.st_mode) != 0;
	}
	if (!is_dir) {
		g.chosen = entry.filename;
		return false;
	}
	change_dir (entry.filename);
	return true;
}

fun handle (wint_t c, bool is_char) -> bool {
	// If an editor is active, let it handle the key instead and eat it
	if (g.editor) {
		handle_editor (c, is_char);
		c = WEOF;
	}

	// Translate the Alt key into a bit outside the range of Unicode
	enum { ALT = 1 << 24 };
	if (c == 27) {
		if (get_wch (&c) == ERR) {
			beep ();
			return true;
		}
		c |= ALT;
	}

	const auto &current = g.entries[g.cursor];
	switch (c) {
	case ALT | L'\r':
	case ALT | KEY_ENTER:
		g.chosen_full = true;
		g.chosen = current.filename;
		return false;
	case L'\r':
	case KEY_ENTER:
		if (choose (current))
			break;
		return false;

	// M-o ought to be the same shortcut the navigator is launched with
	case ALT | L'o':
	case L'q':
		return false;

	case L'k': case CTRL L'p': case KEY_UP:
		g.cursor--;
		break;
	case L'j': case CTRL L'n': case KEY_DOWN:
		g.cursor++;
		break;
	case L'g': case ALT | L'<': case KEY_HOME:
		g.cursor = 0;
		break;
	case L'G': case ALT | L'>': case KEY_END:
		g.cursor = int (g.entries.size ()) - 1;
		break;

	case KEY_PPAGE: g.cursor -= LINES; break;
	case KEY_NPAGE: g.cursor += LINES; break;

	case CTRL L'e': g.offset++; break;
	case CTRL L'y': g.offset--; break;

	case '&':
		change_dir (g.start_dir);
		break;
	case '~':
		if (const auto *home = getenv ("HOME"))
			change_dir (home);
		else if (const auto *pw = getpwuid (getuid ()))
			change_dir (pw->pw_dir);
		break;

	case L't':
	case ALT | L't':
		g.full_view = !g.full_view;
		break;

	case ALT | L'e':
		g.editor_line = to_wide (current.filename);
		// Fall-through
	case L'e':
		g.editor = c & ~ALT;
		break;
	case L'/':
	case L's':
		g.editor = c;
		break;

	case CTRL L'L':
		clear ();
		break;
	case L'r':
		reload ();
		break;
	case KEY_RESIZE:
	case WEOF:
		break;
	default:
		beep ();
	}
	g.cursor = max (g.cursor, 0);
	g.cursor = min (g.cursor, int (g.entries.size ()) - 1);

	// Decrease the offset when more items can suddenly fit
	int pushable = visible_lines () - (int (g.entries.size ()) - g.offset);
	g.offset -= max (pushable, 0);

	// Make sure cursor is visible
	g.offset = max (g.offset, 0);
	g.offset = min (g.offset, int (g.entries.size ()) - 1);

	if (g.offset > g.cursor)
		g.offset = g.cursor;
	if (g.cursor - g.offset >= visible_lines ())
		g.offset = g.cursor - visible_lines () + 1;

	update ();
	return true;
}

fun inotify_check () {
	// Only provide simple indication that contents might have changed
	char buf[4096]; ssize_t len;
	bool changed = false;
	while ((len = read (g.inotify_fd, buf, sizeof buf)) > 0) {
		const inotify_event *e;
		for (char *ptr = buf; ptr < buf + len; ptr += sizeof *e + e->len) {
			e = (const inotify_event *) buf;
			if (e->wd == g.inotify_wd)
				changed = g.out_of_date = true;
		}
	}
	if (changed)
		update ();
}

fun decode_ansi_sgr (const vector<string> &v) -> chtype {
	vector<int> args;
	for (const auto &arg : v) {
		char *end; unsigned long ul = strtoul (arg.c_str (), &end, 10);
		if (*end != '\0' || ul > 255)
			return 0;
		args.push_back (ul);
	}
	chtype result = 0; int fg = -1, bg = -1;
	for (size_t i = 0; i < args.size (); i++) {
		auto arg = args[i];
		if (arg == 0) {
			result = 0; fg = -1; bg = -1;
		} else if (arg == 1) {
			result |= A_BOLD;
		} else if (arg == 4) {
			result |= A_UNDERLINE;
		} else if (arg == 5) {
			result |= A_BLINK;
		} else if (arg == 7) {
			result |= A_REVERSE;
		} else if (arg >= 30 && arg <= 37) {
			fg = arg - 30;
		} else if (arg >= 40 && arg <= 47) {
			bg = arg - 40;
		// Anything other than indexed colours will be rejected completely
		} else if (arg == 38 && (i += 2) < args.size ()) {
			if (args[i - 1] != 5 || (fg = args[i]) >= COLORS)
				return 0;
		} else if (arg == 48 && (i += 2) < args.size ()) {
			if (args[i - 1] != 5 || (bg = args[i]) >= COLORS)
				return 0;
		}
	}
	if (fg != -1 || bg != -1)
		result |= COLOR_PAIR (allocate_pair (fg, bg));
	return result;
}

fun load_ls_colors (vector<string> colors) {
	map<string, chtype> attrs;
	for (const auto &pair : colors) {
		auto equal = pair.find ('=');
		if (equal == string::npos)
			continue;
		attrs[pair.substr (0, equal)] =
			decode_ansi_sgr (split (pair.substr (equal + 1), ";"));
	}

	// `LINK target` i.e. `ln=target` is not supported now
	for (int i = 0; i < LS_COUNT; i++) {
		auto m = attrs.find (g_ls_colors[i]);
		if (m != attrs.end ())
			g.ls_colors[i] = m->second;
	}
	for (const auto &pair : attrs) {
		if (pair.first.substr (0, 2) == "*.")
			g.ls_exts[pair.first.substr (2)] = pair.second;
	}
}

fun load_configuration () {
	auto config = xdg_config_find ("/" PROJECT_NAME "/look");
	if (!config)
		return;

	// Bail out on dumb terminals, there's not much one can do about them
	if (!has_colors () || start_color () == ERR || use_default_colors () == ERR)
		return;

	string line;
	while (getline (*config, line)) {
		auto tokens = split (line, " ");
		if (tokens.empty () || line.front () == '#')
			continue;
		auto name = shift (tokens);
		for (int i = 0; i < g.AT_COUNT; i++)
			if (name == g.attr_names[i])
				g.attrs[i] = decode_attrs (tokens);
	}

	if (const char *colors = getenv ("LS_COLORS"))
		load_ls_colors (split (colors, ":"));
}

int main (int argc, char *argv[]) {
	(void) argc;
	(void) argv;

	// That bitch zle closes stdin before exec without redirection
	(void) close (STDIN_FILENO);
	if (open ("/dev/tty", O_RDWR)) {
		cerr << "cannot open tty" << endl;
		return 1;
	}

	// Save the original stdout and force ncurses to use the terminal directly
	auto output_fd = dup (STDOUT_FILENO);
	dup2 (STDIN_FILENO, STDOUT_FILENO);

	if ((g.inotify_fd = inotify_init1 (IN_NONBLOCK)) < 0) {
		cerr << "cannot initialize inotify" << endl;
		return 1;
	}

	locale::global (locale (""));
	if (!initscr () || cbreak () == ERR || noecho () == ERR || nonl () == ERR) {
		cerr << "cannot initialize screen" << endl;
		return 1;
	}

	load_configuration ();
	reload ();
	g.start_dir = g.cwd;
	update ();

	// Invoking keypad() earlier would make ncurses flush its output buffer,
	// which would worsen start-up flickering
	if (halfdelay (1) == ERR || keypad (stdscr, TRUE) == ERR) {
		endwin ();
		cerr << "cannot initialize screen" << endl;
		return 1;
	}

	wint_t c;
	while (1) {
		inotify_check ();
		int res = get_wch (&c);
		if (res != ERR && !handle (c, res == OK))
			break;
	}
	endwin ();

	// Presumably it is going to end up as an argument, so quote it
	if (!g.chosen.empty ())
		g.chosen = shell_escape (g.chosen);

	// We can't portably create a standard stream from an FD, so modify the FD
	dup2 (output_fd, STDOUT_FILENO);

	if (g.chosen_full) {
		auto full_path = g.cwd + "/" + g.chosen;
		cout << "local insert=" << shell_escape (full_path) << endl;
		return 0;
	}
	if (g.cwd != g.start_dir)
		cout << "local cd=" << shell_escape (g.cwd) << endl;
	if (!g.chosen.empty ())
		cout << "local insert=" << shell_escape (g.chosen) << endl;
	return 0;
}
