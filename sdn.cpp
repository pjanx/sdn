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

// --- Application -------------------------------------------------------------

#define CTRL 31 &

struct entry {
	string filename; struct stat info;
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

	string chosen;                      // Chosen item for the command line
	bool chosen_full;                   // Use the full path

	int inotify_fd, inotify_wd = -1;    // File watch
	bool out_of_date;                   // Entries may be out of date

	wchar_t editor;                     // Prompt character for editing
	wstring editor_line;                // Current user input
} g;

fun make_row (const entry &entry) -> vector<ncstring> {
	vector<ncstring> result;
	const auto &info = entry.info;
	result.push_back (apply_attrs (decode_mode (info.st_mode), 0));

	if (auto u = getpwuid (info.st_uid))
		result.push_back (apply_attrs (to_wide (u->pw_name), 0));
	else
		result.push_back (apply_attrs (to_wstring (info.st_uid), 0));

	if (auto g = getgrgid (info.st_gid))
		result.push_back (apply_attrs (to_wide (g->gr_name), 0));
	else
		result.push_back (apply_attrs (to_wstring (info.st_gid), 0));

	// TODO: human-readable
	result.push_back (apply_attrs (to_wstring (info.st_size), 0));

	auto now = time (NULL);
	auto now_year = localtime (&now)->tm_year;

	char buf[32] = "";
	auto tm = localtime (&info.st_mtime);
	strftime (buf, sizeof buf,
		(tm->tm_year == now_year) ? "%b %e %H:%M" : "%b %e  %Y", tm);
	result.push_back (apply_attrs (to_wide (buf), 0));

	// TODO: symlink target and whatever formatting
	result.push_back (apply_attrs (to_wide (entry.filename), 0));
	return result;
}

fun inline visible_lines () -> int { return max (0, LINES - 2); }

fun update () {
	erase ();

	int available = visible_lines ();
	int used = min (available, int (g.entries.size ()) - g.offset);
	for (int i = 0; i < used; i++) {
		attrset (0);
		int index = g.offset + i;
		if (index == g.cursor)
			attron (A_REVERSE);

		// TODO: use g.full_view, align properly (different columns differently)
		// XXX: maybe this should return a struct instead
		auto row = make_row (g.entries[index]);

		move (available - used + i, 0);
		auto limit = COLS, used = 0;
		for (auto &i : row) {
			used += print (i, limit - used);
			used += print (apply_attrs (L" ", 0), limit - used);
		}
		hline (' ', limit - used);
	}

	attrset (A_BOLD);
	mvprintw (LINES - 2, 0, "%s", g.cwd.c_str ());
	if (g.out_of_date)
		addstr (" [+]");

	attrset (0);
	if (g.editor) {
		move (LINES - 1, 0);
		wchar_t prefix[] = { g.editor, L' ', L'\0' };
		addwstr (prefix);
		move (LINES - 1, print (apply_attrs (g.editor_line, 0), COLS - 3) + 2);
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
		g.entries.push_back ({ f->d_name, sb });
	}
	closedir (dir);
	sort (begin (g.entries), end (g.entries));
	g.out_of_date = false;

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
