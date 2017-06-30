//
// sdn: simple directory navigator
//
// Copyright (c) 2017, Přemysl Janouch <p.janouch@gmail.com>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

#include <string>
#include <vector>
#include <locale>
#include <iostream>
#include <algorithm>
#include <cwchar>
#include <climits>
#include <cstdlib>
#include <fstream>

#include <ncurses.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

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
	setcchar (&ch, &c, attrs, 0, nullptr);
	return ch;
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
	string filename; struct stat info; row row;
	auto operator< (const entry &other) -> bool {
		auto a = S_ISDIR (info.st_mode);
		auto b = S_ISDIR (other.info.st_mode);
		return (a && !b) || (a == b && filename < other.filename);
	}
};

static struct {
	string cwd;                         // Current working directory
	vector<entry> entries;              // Current directory entries
	int offset, cursor;                 // Scroll offset and cursor position
	bool full_view;                     // Whether to show extended information
	int max_widths[row::COLUMNS];       // Column widths

	string chosen;                      // Chosen item for the command line
	bool chosen_full;                   // Use the full path

	int inotify_fd, inotify_wd = -1;    // File watch
	bool out_of_date;                   // Entries may be out of date

	wchar_t editor;                     // Prompt character for editing
	wstring editor_line;                // Current user input

	enum { AT_CURSOR, AT_BAR, AT_CWD, AT_INPUT, AT_COUNT };
	chtype attrs[AT_COUNT] = {A_REVERSE, 0, A_BOLD, 0};
	const char *attr_names[AT_COUNT] = {"cursor", "bar", "cwd", "input"};
} g;

fun make_row (const string &filename, const struct stat &info) -> row {
	row r;
	r.cols[row::MODES] = apply_attrs (decode_mode (info.st_mode), 0);

	auto user = to_wstring (info.st_uid);
	if (auto u = getpwuid (info.st_uid))
		user = to_wide (u->pw_name);
	r.cols[row::USER] = apply_attrs (user, 0);

	auto group = to_wstring (info.st_gid);
	if (auto g = getgrgid (info.st_gid))
		group = to_wide (g->gr_name);
	r.cols[row::GROUP] = apply_attrs (group, 0);

	auto size = to_wstring (info.st_size);
	if      (info.st_size >> 40) size = to_wstring (info.st_size >> 40) + L"T";
	else if (info.st_size >> 30) size = to_wstring (info.st_size >> 30) + L"G";
	else if (info.st_size >> 20) size = to_wstring (info.st_size >> 20) + L"M";
	else if (info.st_size >> 10) size = to_wstring (info.st_size >> 10) + L"K";
	r.cols[row::SIZE] = apply_attrs (size, 0);

	auto now = time (NULL);
	auto now_year = localtime (&now)->tm_year;

	char buf[32] = "";
	auto tm = localtime (&info.st_mtime);
	strftime (buf, sizeof buf,
		(tm->tm_year == now_year) ? "%b %e %H:%M" : "%b %e  %Y", tm);
	r.cols[row::MTIME] = apply_attrs (to_wide (buf), 0);

	// TODO: symlink target and whatever formatting
	r.cols[row::FILENAME] = apply_attrs (to_wide (filename), 0);
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
		attrset (index == g.cursor ? g.attrs[g.AT_CURSOR] : 0);
		move (available - used + i, 0);

		auto used = 0;
		for (int col = start_column; col < row::COLUMNS; col++) {
			const auto &field = g.entries[index].row.cols[col];
			auto aligned = align (field, alignment[col] * g.max_widths[col]);
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
	char buf[4096]; g.cwd = getcwd (buf, sizeof buf);

	auto dir = opendir (".");
	g.entries.clear ();
	while (auto f = readdir (dir)) {
		// Two dots are for navigation but this ain't as useful
		if (f->d_name == string ("."))
			continue;

		struct stat sb = {};
		lstat (f->d_name, &sb);
		g.entries.push_back ({ f->d_name, sb, make_row (f->d_name, sb) });
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
	update ();

	if (g.inotify_wd != -1)
		inotify_rm_watch (g.inotify_fd, g.inotify_wd);

	g.inotify_wd = inotify_add_watch (g.inotify_fd, buf,
		IN_ALL_EVENTS | IN_ONLYDIR);
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
	{
		bool is_dir = S_ISDIR (current.info.st_mode) != 0;
		// Dive into directories and accessible symlinks to them
		if (S_ISLNK (current.info.st_mode)) {
			char buf[PATH_MAX];
			struct stat sb = {};
			auto len = readlink (current.filename.c_str (), buf, sizeof buf);
			is_dir = len > 0 && size_t (len) < sizeof buf
				&& !stat (current.filename.c_str (), &sb)
				&& S_ISDIR (sb.st_mode) != 0;
		}
		if (!is_dir) {
			g.chosen = current.filename;
			return false;
		}
		if (!chdir (current.filename.c_str ())) {
			g.cursor = 0;
			reload ();
		}
		break;
	}

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
				changed = true;
		}
	}
	if (changed)
		update ();
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
		vector<string> tokens = split (line, " ");
		if (tokens.empty () || line.front () == '#')
			continue;
		auto name = shift (tokens);
		for (int i = 0; i < g.AT_COUNT; i++)
			if (name == g.attr_names[i])
				g.attrs[i] = decode_attrs (tokens);
	}

	// TODO: load and use LS_COLORS
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
	if (!initscr () || cbreak () == ERR || noecho () == ERR || nonl () == ERR
	 || halfdelay (1) == ERR || keypad (stdscr, TRUE) == ERR) {
		cerr << "cannot initialize screen" << endl;
		return 1;
	}

	load_configuration ();
	reload ();
	auto start_dir = g.cwd;

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
	if (g.cwd != start_dir)
		cout << "local cd=" << shell_escape (g.cwd) << endl;
	if (!g.chosen.empty ())
		cout << "local insert=" << shell_escape (g.chosen) << endl;
	return 0;
}
