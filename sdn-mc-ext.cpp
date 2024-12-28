//
// sdn-mc-ext: Midnight Commander extension file processor
//
// Copyright (c) 2024, Přemysl Eric Janouch <p@janouch.name>
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

#include <cstdlib>
#include <cctype>
#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

// Trailing return types make C++ syntax suck considerably less
#define fun static auto

using namespace std;

// It is completely fine if this only modifies ASCII letters.
fun tolower (const string &s) -> string {
    string result;
	for (auto c : s) result += tolower (c);
    return result;
}

fun shell_escape (const string &v) -> string {
    return "'" + regex_replace (v, regex {"'"}, "'\\''") + "'";
}

string arg_type, arg_path, arg_basename, arg_dirname, arg_verb;
unordered_map<string, unordered_map<string, string>> sections;

fun expand_command (string command) -> pair<string, string> {
	regex re_sequence {R"(%(%|[[:alpha:]]*\{([^}]*)\}|[[:alpha:]]+))"};
	regex re_name {R"([^{}]*)"};
	regex re_parameter {R"([^,]+")"};
	string kind, out, pipe; smatch m;
	while (regex_search (command, m, re_sequence)) {
		out.append (m.prefix ());
		auto seq = m.str (1);
		command = m.suffix ();

		string argument = m.str (2);
		if (regex_search (seq, m, re_name))
			seq = m.str ();

		if (seq == "%") {
			out += "%";
		} else if (seq == "p") {
			out += shell_escape (arg_basename);
		} else if (seq == "f") {
			out += shell_escape (arg_path);
		} else if (seq == "d") {
			out += shell_escape (arg_dirname);
		} else if (seq == "var") {
			string value;
			if (auto colon = argument.find (':'); colon == argument.npos) {
				if (auto v = getenv (argument.c_str ()))
					value = v;
			} else {
				value = argument.substr (colon + 1);
				if (auto v = getenv (argument.substr (0, colon).c_str ()))
					value = v;
			}
			out += shell_escape (value);
		} else if (seq == "cd") {
			kind = seq;
			command = regex_replace (command, regex {"^ +"}, "");
		} else if (seq == "view") {
			kind = seq;
			command = regex_replace (command, regex {"^ +"}, "");

			sregex_token_iterator it (argument.begin (), argument.end (),
				re_parameter, 0), end;
			for (; it != end; it++) {
				if (*it == "hex")
					pipe.append (" | od -t x1");

				// more(1) and less(1) either ignore or display this:
				//if (*it == "nroff")
				//	pipe.append (" | col -b");
			}
		} else if (seq == "") {
			cerr << "sdn-mc-ext: prompting not supported" << endl;
			return {};
		} else {
			cerr << "sdn-mc-ext: unsupported: %" << seq << endl;
			return {};
		}
	}
	return {kind,
		pipe.empty () ? out.append (command) : "(" + out + ")" + pipe};
}

fun print_command (string cmd) {
	auto command = expand_command (cmd);
	cout << get<0> (command) << endl << get<1> (command) << endl;
}

fun section_matches (const unordered_map<string, string> &section) -> bool {
	if (section.count ("Directory"))
		return false;

	// The configuration went through some funky changes;
	// unescape \\ but leave other escapes alone.
	auto filter_re = [](const string &s) {
		string result;
		for (size_t i = 0; i < s.length (); ) {
			auto c = s[i++];
			if (c == '\\' && i < s.length ())
				if (c = s[i++]; c != '\\')
					result += '\\';
			result += c;
		}
		return result;
	};
	auto is_true = [&](const string &name) {
		auto value = section.find (name);
		return value != section.end () && value->second == "true";
	};
	if (auto kv = section.find ("Type"); kv != section.end ()) {
		auto flags = std::regex::ECMAScript;
		if (is_true ("TypeIgnoreCase"))
			flags |= regex_constants::icase;
		if (!regex_search (arg_type, regex {filter_re (kv->second), flags}))
			return false;
	}
	auto basename = arg_basename;
	if (auto kv = section.find ("Regex"); kv != section.end ()) {
		auto flags = std::regex::ECMAScript;
		if (is_true ("RegexIgnoreCase"))
			flags |= regex_constants::icase;
		return regex_search (basename, regex {filter_re (kv->second), flags});
	}
	if (auto kv = section.find ("Shell"); kv != section.end ()) {
		auto value = kv->second;
		if (is_true ("ShellIgnoreCase")) {
			value = tolower (value);
			basename = tolower (arg_basename);
		}
		if (value.empty () || value[0] != '.')
			return value == basename;
		return basename.length () >= value.length () &&
			basename.substr (basename.length () - value.length ()) == value;
	}
	return !arg_type.empty ();
}

fun process (const string &section) -> bool {
	auto full = sections.at (section);
	if (auto include = full.find ("Include"); include != full.end ()) {
		full.erase ("Open");
		full.erase ("View");
		full.erase ("Edit");

		if (auto included = sections.find ("Include/" + include->second);
			included != sections.end ()) {
			for (const auto &kv : included->second)
				full[kv.first] = kv.second;
		}
	}
	if (getenv ("SDN_MC_EXT_DEBUG")) {
		cerr << "[" << section << "]" << endl;
		for (const auto &kv : full)
			cerr << "  " << kv.first << ": " << kv.second << endl;
	}
	if (full.count (arg_verb) && section_matches (full)) {
		print_command (full[arg_verb]);
		return true;
	}
	return false;
}

int main (int argc, char *argv[]) {
	if (argc != 6) {
		cerr << "Usage: " << argv[0]
			<< " TYPE PATH BASENAME DIRNAME VERB < mc.ext.ini" << endl;
		return 2;
	}

	arg_type = argv[1];
	arg_path = argv[2], arg_basename = argv[3], arg_dirname = argv[4];
	arg_verb = argv[5];

	string line, section;
	vector<string> order;
	regex re_entry {R"(^([-\w]+) *= *(.*)$)"};
	smatch m;
	while (getline (cin, line)) {
		if (line.empty () || line[0] == '#') {
			continue;
		} else if (auto length = line.length();
			line.find_last_of ('[') == 0 &&
			line.find_first_of (']') == length - 1) {
			order.push_back ((section = line.substr (1, length - 2)));
		} else if (regex_match (line, m, re_entry)) {
			sections[section][m[1]] = m[2];
		}
	}
	for (const auto &section : order) {
		if (section == "mc.ext.ini" ||
			section == "Default" ||
			section.substr (0, 8) == "Include/")
			continue;
		if (process (section))
			return 0;
	}
	print_command (sections["Default"][arg_verb]);
	return 0;
}
