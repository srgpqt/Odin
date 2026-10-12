// Cross-compilation support with on-demand libc builds.
//
// The compiler ships the sources (or, for libcs which are only ever linked dynamically, the
// symbol lists) of the C libraries of the targets it supports in `<ODIN_ROOT>/libc`:
//
//     glibc        start files + `abilists`, a list of every symbol of every glibc version
//     musl         the whole source tree
//     mingw-w64    CRT sources + the `.def` files of the Windows DLLs
//     FreeBSD      start files + `abilists`
//     NetBSD       start files + `abilists`
//     Darwin       `libSystem.tbd`
//     compiler-rt  the builtins (`__udivdi3`, `__addtf3`, ...) the libcs and LLVM rely on
//
// When a program gets linked against one of them, the pieces which are needed are built for
// the exact target with `clang` and stored in a global cache, so each of them is only ever
// built once. The final link is then done by calling LLD directly, as there is no system
// toolchain for the target that knows where its start files and libraries are.
//
// The approach, the `abilists` format and the build recipes are those of the Zig compiler,
// see `libc/README.md`.

#if !defined(GB_SYSTEM_WINDOWS)
#include <spawn.h>
extern char **environ;
#endif

// NOTE: bump this when the way a cached artefact is built changes
#define CROSS_LIBC_CACHE_REVISION 1

struct CrossTarget {
	TargetOsKind   os;
	TargetArchKind arch;
	LibcKind       libc;
	LibcVersion    version;     // glibc version, or the OS version for the BSDs and Darwin
	isize          ptr_size;

	String cc_triple;           // what `clang --target=` gets
	String cc_flags;            // flags every C/assembly file of this target is compiled with
	String tag;                 // unique name of the target, used for the cache
	String rt_tag;              // the same without the things compiler-rt does not depend on
};

struct CrossTools {
	String cc;
	String lld;
};

gb_global CrossTools cross_tools;
gb_global String     cross_libc_root; // `<ODIN_ROOT>/libc/`

////////////////////////////////////////////////////////////////////////////////////////////
// Utilities

gb_internal String cross_fmt(char const *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	va_list va2;
	va_copy(va2, va);
	isize len = vsnprintf(nullptr, 0, fmt, va);
	va_end(va);
	char *buf = gb_alloc_array(heap_allocator(), char, len+1);
	vsnprintf(buf, len+1, fmt, va2);
	va_end(va2);
	return make_string(cast(u8 *)buf, len);
}

gb_internal char const *cross_cstr(String const &s) {
	return alloc_cstring(heap_allocator(), s);
}

gb_internal bool cross_file_exists(String const &path) {
	char const *c = cross_cstr(path);
	defer (gb_free(heap_allocator(), cast(void *)c));
	return gb_file_exists(c);
}

gb_internal bool cross_make_directory(String const &path) {
	if (path.len == 0 || path_is_directory(path)) {
		return true;
	}
	// Create the parents first
	isize i = path.len-1;
	while (i > 0 && (path[i] == '/' || path[i] == '\\')) i--;
	while (i > 0 && path[i] != '/' && path[i] != '\\') i--;
	if (i > 0) {
		if (!cross_make_directory(substring(path, 0, i))) {
			return false;
		}
	}
	check_if_exists_directory_otherwise_create(path);
	return path_is_directory(path);
}

gb_internal bool cross_write_file(String const &path, void const *data, isize len) {
	char const *c = cross_cstr(path);
	defer (gb_free(heap_allocator(), cast(void *)c));
	gbFile f = {};
	if (gb_file_create(&f, c) != gbFileError_None) {
		gb_printf_err("Failed to create '%.*s'\n", LIT(path));
		return false;
	}
	defer (gb_file_close(&f));
	if (len > 0 && !gb_file_write(&f, data, len)) {
		gb_printf_err("Failed to write '%.*s'\n", LIT(path));
		return false;
	}
	return true;
}

gb_internal bool cross_read_file(String const &path, String *contents) {
	char const *c = cross_cstr(path);
	defer (gb_free(heap_allocator(), cast(void *)c));
	gbFileContents fc = gb_file_read_contents(heap_allocator(), true, c);
	if (fc.data == nullptr) {
		return false;
	}
	*contents = make_string(cast(u8 *)fc.data, fc.size);
	return true;
}

gb_internal void cross_remove_directory_tree(String const &path) {
	Array<FileInfo> list = {};
	if (read_directory(path, &list) == ReadDirectory_None) {
		for (FileInfo const &fi : list) {
			if (fi.is_dir) {
				cross_remove_directory_tree(fi.fullpath);
			} else {
				char const *c = cross_cstr(fi.fullpath);
				gb_file_remove(c);
				gb_free(heap_allocator(), cast(void *)c);
			}
		}
		array_free(&list);
	}
	char const *c = cross_cstr(path);
	defer (gb_free(heap_allocator(), cast(void *)c));
#if defined(GB_SYSTEM_WINDOWS)
	String16 w = string_to_string16(heap_allocator(), path);
	RemoveDirectoryW(cast(wchar_t *)w.text);
#else
	rmdir(c);
#endif
}

gb_internal bool cross_rename(String const &from, String const &to) {
#if defined(GB_SYSTEM_WINDOWS)
	String16 wfrom = string_to_string16(heap_allocator(), from);
	String16 wto   = string_to_string16(heap_allocator(), to);
	return MoveFileW(cast(wchar_t *)wfrom.text, cast(wchar_t *)wto.text) != 0;
#else
	char const *cfrom = cross_cstr(from);
	char const *cto   = cross_cstr(to);
	defer (gb_free(heap_allocator(), cast(void *)cfrom));
	defer (gb_free(heap_allocator(), cast(void *)cto));
	return rename(cfrom, cto) == 0;
#endif
}

gb_internal String cross_env(char const *name) {
	char const *v = gb_get_env(name, permanent_allocator());
	if (v == nullptr) {
		return {};
	}
	return make_string_c(v);
}

// Looks for the first of the `names` which is an executable in one of the directories of `PATH`
gb_internal String cross_find_in_path(String const *names, isize name_count) {
	String path = cross_env("PATH");
#if defined(GB_SYSTEM_WINDOWS)
	char const sep = ';';
	String exts[] = {str_lit(".exe"), str_lit("")};
#else
	char const sep = ':';
	String exts[] = {str_lit("")};
#endif
	for (isize n = 0; n < name_count; n++) {
		String_Iterator it = {path, 0};
		String dir = {};
		while (string_split_iterator_next(&it, sep, &dir)) {
			if (dir.len == 0) {
				continue;
			}
			for (String const &ext : exts) {
				String candidate = cross_fmt("%.*s/%.*s%.*s", LIT(dir), LIT(names[n]), LIT(ext));
				if (cross_file_exists(candidate) && !path_is_directory(candidate)) {
					return candidate;
				}
				gb_free(heap_allocator(), candidate.text);
			}
		}
	}
	return {};
}

gb_internal bool cross_find_cc(void) {
	if (cross_tools.cc.len != 0) {
		return true;
	}
	String cc = cross_env("ODIN_CLANG_PATH");
	if (cc.len == 0) {
		String names[] = {
			str_lit("clang"),
			cross_fmt("clang-%d", LLVM_VERSION_MAJOR),
		};
		cc = cross_find_in_path(names, gb_count_of(names));
	}
	if (cc.len == 0) {
		gb_printf_err("Could not find `clang`, which is needed to build the C library of the target.\n");
		gb_printf_err("\tSuggestion: install clang, or set the ODIN_CLANG_PATH environment variable\n");
		return false;
	}
	cross_tools.cc = cc;
	return true;
}

gb_internal bool cross_find_lld(void) {
	if (cross_tools.lld.len != 0) {
		return true;
	}

	// NOTE: every LLD executable is able to be any of the linkers with `-flavor`
	String lld = cross_env("ODIN_LLD_PATH");
	if (lld.len == 0) {
	#if defined(GB_SYSTEM_WINDOWS)
		lld = cross_fmt("%.*s\\bin\\lld-link.exe", LIT(build_context.ODIN_ROOT));
		if (!cross_file_exists(lld)) {
			lld = {};
		}
	#endif
	}
	if (lld.len == 0) {
		String names[] = {
			str_lit("lld"),
			str_lit("ld.lld"),
			str_lit("lld-link"),
			str_lit("ld64.lld"),
			str_lit("wasm-ld"),
			cross_fmt("lld-%d", LLVM_VERSION_MAJOR),
			cross_fmt("ld.lld-%d", LLVM_VERSION_MAJOR),
		};
		lld = cross_find_in_path(names, gb_count_of(names));
	}
	if (lld.len == 0) {
		gb_printf_err("Could not find LLD, which is needed to link for other platforms.\n");
		gb_printf_err("\tSuggestion: install lld, or set the ODIN_LLD_PATH environment variable\n");
		return false;
	}

	cross_tools.lld = lld;
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Running commands

gb_internal i32 cross_exec(String const &cmd) {
	if (build_context.show_system_calls) {
		gb_printf_err("[SYSTEM CALL] %.*s\n", LIT(cmd));
	}

#if defined(GB_SYSTEM_WINDOWS)
	STARTUPINFOW start_info = {gb_size_of(STARTUPINFOW)};
	PROCESS_INFORMATION proc_info = {0};
	start_info.dwFlags    = STARTF_USESTDHANDLES;
	start_info.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
	start_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	start_info.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

	String16 wcmd = string_to_string16(heap_allocator(), cmd);
	defer (gb_free(heap_allocator(), wcmd.text));

	DWORD exit_code = 1;
	if (!CreateProcessW(nullptr, cast(wchar_t *)wcmd.text, nullptr, nullptr, true, 0, nullptr, nullptr, &start_info, &proc_info)) {
		gb_printf_err("Failed to execute command:\n\t%.*s\n", LIT(cmd));
		return -1;
	}
	WaitForSingleObject(proc_info.hProcess, INFINITE);
	GetExitCodeProcess(proc_info.hProcess, &exit_code);
	CloseHandle(proc_info.hProcess);
	CloseHandle(proc_info.hThread);
	return cast(i32)exit_code;
#else
	char const *c = cross_cstr(cmd);
	defer (gb_free(heap_allocator(), cast(void *)c));

	int argc = 0;
	char **argv = command_line_to_spawn_argv(c, &argc);
	defer (gb_free(heap_allocator(), argv));
	if (argc == 0) {
		return -1;
	}

	pid_t pid = 0;
	int status = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv, environ);
	if (status != 0) {
		gb_printf_err("Failed to execute '%s': %s\n", argv[0], strerror(status));
		return -1;
	}
	for (;;) {
		if (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			return 128 + WTERMSIG(status);
		}
	}
#endif
}

struct CrossJobs {
	Array<String>    commands;
	std::atomic<i32> failed;
	BlockingMutex    mutex;
	String           first_failure;
};

struct CrossJobTask {
	CrossJobs *jobs;
	isize      index;
};

gb_internal void cross_jobs_init(CrossJobs *jobs) {
	array_init(&jobs->commands, heap_allocator(), 0, 64);
	jobs->failed.store(0);
	jobs->first_failure = {};
}

gb_internal void cross_jobs_add(CrossJobs *jobs, String const &cmd) {
	array_add(&jobs->commands, cmd);
}

gb_internal WORKER_TASK_PROC(cross_job_worker_proc) {
	CrossJobTask *task = cast(CrossJobTask *)data;
	CrossJobs *jobs = task->jobs;
	if (jobs->failed.load() != 0) {
		return 0;
	}
	String cmd = jobs->commands[task->index];
	if (cross_exec(cmd) != 0) {
		mutex_lock(&jobs->mutex);
		if (jobs->failed.load() == 0) {
			jobs->first_failure = cmd;
		}
		jobs->failed.store(1);
		mutex_unlock(&jobs->mutex);
	}
	return 0;
}

// Runs all the commands in parallel
gb_internal bool cross_jobs_run(CrossJobs *jobs) {
	CrossJobTask *tasks = gb_alloc_array(heap_allocator(), CrossJobTask, jobs->commands.count);
	defer (gb_free(heap_allocator(), tasks));

	for_array(i, jobs->commands) {
		tasks[i].jobs  = jobs;
		tasks[i].index = i;
		thread_pool_add_task(&global_thread_pool, cross_job_worker_proc, &tasks[i]);
	}
	thread_pool_wait(&global_thread_pool);

	bool ok = jobs->failed.load() == 0;
	if (!ok) {
		gb_printf_err("The following command failed:\n\t%.*s\n", LIT(jobs->first_failure));
	}
	array_clear(&jobs->commands);
	return ok;
}

////////////////////////////////////////////////////////////////////////////////////////////
// The global cache

gb_internal String cross_global_cache_directory(void) {
	gb_local_persist String dir = {};
	if (dir.len != 0) {
		return dir;
	}

	String env = cross_env("ODIN_CACHE_DIR");
	if (env.len != 0) {
		dir = env;
	} else {
	#if defined(GB_SYSTEM_WINDOWS)
		String base = cross_env("LOCALAPPDATA");
		if (base.len == 0) {
			base = temporary_directory(permanent_allocator());
		}
		dir = cross_fmt("%.*s/odin/cache", LIT(base));
	#elif defined(GB_SYSTEM_OSX)
		dir = cross_fmt("%.*s/Library/Caches/odin", LIT(cross_env("HOME")));
	#else
		String base = cross_env("XDG_CACHE_HOME");
		if (base.len != 0) {
			dir = cross_fmt("%.*s/odin", LIT(base));
		} else {
			String home = cross_env("HOME");
			if (home.len == 0) {
				home = str_lit("/tmp");
			}
			dir = cross_fmt("%.*s/.cache/odin", LIT(home));
		}
	#endif
	}
	dir = normalize_path(permanent_allocator(), dir, NIX_SEPARATOR_STRING);
	return dir;
}

// An artefact in the cache is a directory. It is built in a temporary directory next to its
// final place and moved there once complete, which makes a build atomic: an interrupted build
// leaves nothing behind that could be mistaken for the artefact, and two compilers building the
// same thing at once is harmless, the one that finishes last throws its result away.
struct CrossCacheEntry {
	String dir;       // the final directory, with a trailing slash
	String build_dir; // where it is built, with a trailing slash
	bool   hit;
};

gb_internal bool cross_cache_begin(String const &name, String const &what, CrossCacheEntry *entry) {
	u64 hash = fnv64a(name.text, name.len);
	hash = fnv64a(ODIN_VERSION.text, ODIN_VERSION.len, hash);
#ifdef GIT_SHA
	hash = fnv64a(GIT_SHA, gb_strlen(GIT_SHA), hash);
#endif
	i32 revision = CROSS_LIBC_CACHE_REVISION;
	hash = fnv64a(&revision, gb_size_of(revision), hash);
	hash = fnv64a(cross_libc_root.text, cross_libc_root.len, hash);
	hash = fnv64a(cross_tools.cc.text, cross_tools.cc.len, hash);

	String root = cross_global_cache_directory();
	entry->dir = cross_fmt("%.*slibc/%.*s-%016llx/", LIT(root), LIT(name), cast(unsigned long long)hash);
	entry->hit = path_is_directory(entry->dir);
	if (entry->hit) {
		entry->build_dir = entry->dir;
		return true;
	}

	gb_local_persist std::atomic<i32> counter;
#if defined(GB_SYSTEM_WINDOWS)
	u64 pid = cast(u64)GetCurrentProcessId();
#else
	u64 pid = cast(u64)getpid();
#endif
	entry->build_dir = cross_fmt("%.*slibc/tmp-%llu-%d-%.*s/", LIT(root), cast(unsigned long long)pid, counter.fetch_add(1), LIT(name));
	cross_remove_directory_tree(entry->build_dir);
	if (!cross_make_directory(entry->build_dir)) {
		gb_printf_err("Failed to create the cache directory '%.*s'\n", LIT(entry->build_dir));
		gb_printf_err("\tSuggestion: set the ODIN_CACHE_DIR environment variable to a writable directory\n");
		return false;
	}

	if (what.len != 0 && !build_context.print_linker_flags) {
		gb_printf_err("Building %.*s (this is only done once)...\n", LIT(what));
	}
	return true;
}

gb_internal bool cross_cache_end(CrossCacheEntry *entry, bool ok) {
	if (entry->hit) {
		return ok;
	}
	if (!ok) {
		if (!build_context.keep_temp_files) {
			cross_remove_directory_tree(entry->build_dir);
		}
		return false;
	}
	if (!cross_rename(entry->build_dir, entry->dir)) {
		// Somebody else was faster
		cross_remove_directory_tree(entry->build_dir);
		if (!path_is_directory(entry->dir)) {
			gb_printf_err("Failed to create the cache directory '%.*s'\n", LIT(entry->dir));
			return false;
		}
	}
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Lists of sources

// The lists of the sources a libc is made of are kept in `sources.txt` files, which are made of
// `[section]` lines followed by one path per line.
struct CrossSourceList {
	String contents;
};

gb_internal bool cross_source_list_load(CrossSourceList *list, String const &path) {
	if (!cross_read_file(path, &list->contents)) {
		gb_printf_err("Failed to read '%.*s', the Odin installation is incomplete\n", LIT(path));
		return false;
	}
	return true;
}

gb_internal void cross_source_list_section(CrossSourceList *list, char const *section, Array<String> *out) {
	String wanted = make_string_c(section);
	bool in_section = false;
	String_Iterator it = {list->contents, 0};
	String line = {};
	while (string_split_iterator_next(&it, '\n', &line)) {
		line = string_trim_whitespace(line);
		if (line.len == 0 || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			in_section = line.len == wanted.len+2 && substring(line, 1, line.len-1) == wanted;
			continue;
		}
		if (in_section) {
			array_add(out, line);
		}
	}
}

// Turns the path of a source file into a name which is unique in a flat directory
gb_internal String cross_object_name(String const &src, char const *ext) {
	String name = cross_fmt("%.*s%s", LIT(src), ext);
	for (isize i = 0; i < src.len; i++) {
		switch (name.text[i]) {
		case '/':
		case '\\':
		case ':':
		case ' ':
			name.text[i] = '_';
			break;
		}
	}
	return name;
}

// A "library" of ELF or Mach-O objects is not put into an archive, which would need an archiver
// for the target, but is given to the linker as a response file which lists the objects between
// `--start-lib` and `--end-lib`. They are treated the very same way as the members of an archive.
gb_internal bool cross_write_lazy_library(String const &path, String const &final_dir, Array<String> const &objects) {
	gbString s = gb_string_make(heap_allocator(), "--start-lib\n");
	defer (gb_string_free(s));
	for (String const &object : objects) {
		s = gb_string_append_fmt(s, "\"%.*s%.*s\"\n", LIT(final_dir), LIT(object));
	}
	s = gb_string_appendc(s, "--end-lib\n");
	return cross_write_file(path, s, gb_string_length(s));
}

////////////////////////////////////////////////////////////////////////////////////////////
// Targets

gb_internal char const *cross_arch_name(TargetArchKind arch) {
	switch (arch) {
	case TargetArch_amd64:   return "x86_64";
	case TargetArch_i386:    return "x86";
	case TargetArch_arm64:   return "aarch64";
	case TargetArch_arm32:   return "arm";
	case TargetArch_riscv64: return "riscv64";
	}
	return "unknown";
}

gb_internal String cross_libc_version_string(LibcVersion const &v) {
	if (v.patch != 0) {
		return cross_fmt("%d.%d.%d", v.major, v.minor, v.patch);
	}
	return cross_fmt("%d.%d", v.major, v.minor);
}

gb_internal int cross_libc_version_compare(LibcVersion const &a, LibcVersion const &b) {
	if (a.major != b.major) return a.major < b.major ? -1 : +1;
	if (a.minor != b.minor) return a.minor < b.minor ? -1 : +1;
	if (a.patch != b.patch) return a.patch < b.patch ? -1 : +1;
	return 0;
}

gb_internal bool cross_target_init(CrossTarget *t) {
	BuildContext *bc = &build_context;
	t->os       = bc->metrics.os;
	t->arch     = bc->metrics.arch;
	t->libc     = bc->libc_kind;
	t->version  = bc->libc_version;
	t->ptr_size = bc->metrics.ptr_size;

	char const *arch = cross_arch_name(t->arch);
	String version = cross_libc_version_string(t->version);

	t->cc_flags = str_lit("");
	switch (t->libc) {
	case Libc_Glibc:
		switch (t->arch) {
		case TargetArch_amd64:   t->cc_triple = str_lit("x86_64-linux-gnu");      break;
		case TargetArch_i386:    t->cc_triple = str_lit("i686-linux-gnu");        break;
		case TargetArch_arm64:   t->cc_triple = str_lit("aarch64-linux-gnu");     break;
		case TargetArch_arm32:   t->cc_triple = str_lit("arm-linux-gnueabihf");   break;
		case TargetArch_riscv64: t->cc_triple = str_lit("riscv64-linux-gnu");     break;
		default: return false;
		}
		t->tag = cross_fmt("%s-linux-glibc.%.*s", arch, LIT(version));
		break;
	case Libc_Musl:
		switch (t->arch) {
		case TargetArch_amd64:   t->cc_triple = str_lit("x86_64-linux-musl");     break;
		case TargetArch_i386:    t->cc_triple = str_lit("i686-linux-musl");       break;
		case TargetArch_arm64:   t->cc_triple = str_lit("aarch64-linux-musl");    break;
		case TargetArch_arm32:   t->cc_triple = str_lit("arm-linux-musleabihf");  break;
		case TargetArch_riscv64: t->cc_triple = str_lit("riscv64-linux-musl");    break;
		default: return false;
		}
		t->tag = cross_fmt("%s-linux-musl", arch);
		break;
	case Libc_Mingw:
		switch (t->arch) {
		case TargetArch_amd64:   t->cc_triple = str_lit("x86_64-w64-windows-gnu"); break;
		default: return false;
		}
		t->tag = cross_fmt("%s-windows-mingw", arch);
		break;
	case Libc_FreeBSD:
		switch (t->arch) {
		case TargetArch_amd64:
		case TargetArch_i386:
		case TargetArch_arm64:
			break;
		default: return false;
		}
		t->cc_triple = cross_fmt("%s-unknown-freebsd%d.%d", t->arch == TargetArch_i386 ? "i386" : arch, t->version.major, t->version.minor);
		t->tag = cross_fmt("%s-freebsd.%.*s", arch, LIT(version));
		break;
	case Libc_NetBSD:
		switch (t->arch) {
		case TargetArch_amd64:
		case TargetArch_arm64:
			break;
		default: return false;
		}
		t->cc_triple = cross_fmt("%s-unknown-netbsd%d.%d", arch, t->version.major, t->version.minor);
		t->tag = cross_fmt("%s-netbsd.%.*s", arch, LIT(version));
		break;
	case Libc_Darwin:
		switch (t->arch) {
		case TargetArch_amd64: t->cc_triple = cross_fmt("x86_64-apple-macosx%.*s", LIT(bc->minimum_os_version_string)); break;
		case TargetArch_arm64: t->cc_triple = cross_fmt("arm64-apple-macosx%.*s",  LIT(bc->minimum_os_version_string)); break;
		default: return false;
		}
		t->tag = cross_fmt("%s-macos", arch);
		break;
	default:
		return false;
	}

	t->rt_tag = cross_fmt("%s-%.*s", arch, LIT(target_os_names[t->os]));

	switch (t->arch) {
	case TargetArch_riscv64:
		t->cc_flags = str_lit("-march=rv64gc -mabi=lp64d");
		break;
	case TargetArch_arm64:
		if (t->os != TargetOs_darwin) {
			// NOTE: the helpers the outlined atomics call are not part of the builtins which get built
			t->cc_flags = str_lit("-mno-outline-atomics");
		}
		break;
	}
	return true;
}

// The common part of every invocation of the C compiler
gb_internal gbString cross_cc_command(CrossTarget *t) {
	gbString cmd = gb_string_make(heap_allocator(), "");
	cmd = gb_string_append_fmt(cmd, "\"%.*s\" --target=%.*s %.*s", LIT(cross_tools.cc), LIT(t->cc_triple), LIT(t->cc_flags));
	// NOTE: `-nostdlibinc` keeps the headers of the compiler itself (`stddef.h`, `stdarg.h`, ...)
	cmd = gb_string_appendc(cmd, " -nostdlibinc -fno-stack-protector -fno-sanitize=all -Qunused-arguments -w");
	return cmd;
}

gb_internal String cross_libc_path(char const *sub_path) {
	return cross_fmt("%.*s%s", LIT(cross_libc_root), sub_path);
}

gb_internal bool cross_is_assembly(String const &path) {
	return string_ends_with(path, str_lit(".s")) || string_ends_with(path, str_lit(".S"));
}

////////////////////////////////////////////////////////////////////////////////////////////
// `abilists`
//
// glibc, FreeBSD's libc and NetBSD's libc are only ever linked dynamically, which means that
// all the linker needs of them is the names (and versions) of the symbols they export. Those
// are stored in a compact form in an `abilists` file, for every target and every version of
// the libc at once. Out of them a tiny assembly file is generated for each of the libraries,
// which defines every symbol the wanted version has, and linked into a shared library which
// stands in for the real one.

enum CrossAbiFlavour {
	CrossAbi_Glibc,
	CrossAbi_FreeBSD,
	CrossAbi_NetBSD,
};

struct CrossAbiLib {
	char const *name;
	i32         sover;
	LibcVersion removed_in; // glibc merged most of its libraries into libc.so
};

// NOTE: the order of these has to be the one of the `abilists` file, it is the order they are linked in too
gb_global CrossAbiLib cross_glibc_libs[] = {
	{"m",       6},
	{"c",       6},
	{"ld",      2},
	{"resolv",  2},
	{"pthread", 0, {2, 34, 0}},
	{"dl",      2, {2, 34, 0}},
	{"rt",      1, {2, 34, 0}},
	{"util",    1, {2, 34, 0}},
};

gb_global CrossAbiLib cross_freebsd_libs[] = {
	{"m",          5},
	{"stdthreads", 0},
	{"thr",        3},
	{"c",          7},
	{"dl",         1},
	{"rt",         1},
	{"ld",         1},
	{"util",       9},
	{"execinfo",   1},
};

gb_global CrossAbiLib cross_netbsd_libs[] = {
	{"m",        0},
	{"pthread",  1},
	{"c",        12},
	{"rt",       1},
	{"ld",       0},
	{"util",     7},
	{"execinfo", 0},
};

struct CrossAbiLists {
	CrossAbiFlavour flavour;
	Slice<CrossAbiLib> libs;
	Array<LibcVersion> versions;
	Array<String>      targets;
	u8 const *inclusions;
	u8 const *end;
};

struct CrossAbiReader {
	u8 const *ptr;
	u8 const *end;
	bool      bad;
};

gb_internal u8 cross_abi_byte(CrossAbiReader *r) {
	if (r->ptr >= r->end) {
		r->bad = true;
		return 0x80;
	}
	return *r->ptr++;
}

gb_internal u64 cross_abi_leb128(CrossAbiReader *r) {
	u64 result = 0;
	for (u32 shift = 0; shift < 64; shift += 7) {
		u8 b = cross_abi_byte(r);
		result |= cast(u64)(b & 0x7f) << shift;
		if ((b & 0x80) == 0 || r->bad) {
			break;
		}
	}
	return result;
}

gb_internal String cross_abi_cstring(CrossAbiReader *r) {
	u8 const *start = r->ptr;
	while (r->ptr < r->end && *r->ptr != 0) {
		r->ptr++;
	}
	String s = make_string(start, r->ptr - start);
	if (r->ptr >= r->end) {
		r->bad = true;
	} else {
		r->ptr++;
	}
	return s;
}

gb_internal bool cross_abilists_load(CrossAbiLists *abi, CrossAbiFlavour flavour, String const &path) {
	String contents = {};
	if (!cross_read_file(path, &contents)) {
		gb_printf_err("Failed to read '%.*s', the Odin installation is incomplete\n", LIT(path));
		return false;
	}
	abi->flavour = flavour;
	switch (flavour) {
	case CrossAbi_Glibc:   abi->libs = {cross_glibc_libs,   gb_count_of(cross_glibc_libs)};   break;
	case CrossAbi_FreeBSD: abi->libs = {cross_freebsd_libs, gb_count_of(cross_freebsd_libs)}; break;
	case CrossAbi_NetBSD:  abi->libs = {cross_netbsd_libs,  gb_count_of(cross_netbsd_libs)};  break;
	}
	array_init(&abi->versions, heap_allocator());
	array_init(&abi->targets,  heap_allocator());

	CrossAbiReader r = {contents.text, contents.text + contents.len, false};

	isize lib_count = cross_abi_byte(&r);
	for (isize i = 0; i < lib_count; i++) {
		String name = cross_abi_cstring(&r);
		if (i >= abi->libs.count || name != make_string_c(abi->libs[i].name)) {
			r.bad = true;
		}
	}
	isize version_count = cross_abi_byte(&r);
	for (isize i = 0; i < version_count; i++) {
		LibcVersion v = {};
		v.major = cross_abi_byte(&r);
		v.minor = cross_abi_byte(&r);
		v.patch = cross_abi_byte(&r);
		array_add(&abi->versions, v);
	}
	isize target_count = cross_abi_byte(&r);
	for (isize i = 0; i < target_count; i++) {
		array_add(&abi->targets, cross_abi_cstring(&r));
	}
	if (r.bad || version_count == 0 || version_count > 128 || target_count > 64) {
		gb_printf_err("'%.*s' is malformed, the Odin installation is corrupt\n", LIT(path));
		return false;
	}
	abi->inclusions = r.ptr;
	abi->end        = r.end;
	return true;
}

gb_internal void cross_abi_version_name(CrossAbiLists *abi, isize index, char *buf, isize buf_len, bool as_symbol) {
	LibcVersion v = abi->versions[index];
	char const *sep = as_symbol ? "_" : ".";
	switch (abi->flavour) {
	case CrossAbi_Glibc:
		if (v.patch == 0) {
			gb_snprintf(buf, buf_len, "%s%d%s%d", as_symbol ? "" : "GLIBC_", v.major, sep, v.minor);
		} else {
			gb_snprintf(buf, buf_len, "%s%d%s%d%s%d", as_symbol ? "" : "GLIBC_", v.major, sep, v.minor, sep, v.patch);
		}
		break;
	default:
		gb_snprintf(buf, buf_len, "FBSD_%d%s%d", v.major, sep, v.minor);
		break;
	}
}

// Generates the assembly which defines every symbol of the library `lib_index` that exists for
// the target `target_index` in the version `version_index` of the libc
gb_internal gbString cross_abilists_generate_stubs(CrossAbiLists *abi, isize lib_index, isize target_index, isize version_index, isize ptr_size) {
	gbString s = gb_string_make_reserve(heap_allocator(), 1<<20);
	char const *word = ptr_size == 8 ? ".quad" : ".long";
	int align = cast(int)ptr_size;

	CrossAbiReader r = {abi->inclusions, abi->end, false};

	enum {Section_Function, Section_Object, Section_TLS, Section_COUNT};
	isize section_count = abi->flavour == CrossAbi_FreeBSD ? 3 : 2;

	for (isize section = 0; section < section_count && !r.bad; section++) {
		char const *type = "%function";
		switch (section) {
		case Section_Function:
			s = gb_string_appendc(s, ".text\n");
			break;
		case Section_Object:
			if (abi->flavour == CrossAbi_Glibc) {
				// The real `libc.so.6` has a weak reference to `_IO_stdin_used`, which makes
				// the linker export it from the executable. Without it glibc thinks that the
				// program was built against an ancient (pre 2.1) version of the `FILE` structure
				s = gb_string_appendc(s, ".rodata\n");
				if (gb_strcmp(abi->libs[lib_index].name, "c") == 0) {
					s = gb_string_append_fmt(s, ".balign %d\n.globl _IO_stdin_used\n%s _IO_stdin_used\n", align, word);
				}
			}
			s = gb_string_appendc(s, ".data\n");
			if (abi->flavour == CrossAbi_FreeBSD && gb_strcmp(abi->libs[lib_index].name, "c") == 0) {
				// These are defined by the start files of the executable
				s = gb_string_append_fmt(s, ".balign %d\n.globl __progname\n.globl environ\n%s __progname\n%s environ\n", align, word, word);
			}
			type = "%object";
			break;
		case Section_TLS:
			s = gb_string_appendc(s, ".tdata\n");
			type = "%tls_object";
			break;
		}

		isize inclusion_count = cross_abi_byte(&r);
		inclusion_count |= cast(isize)cross_abi_byte(&r) << 8;

		// The state of the symbol the inclusions which are being read belong to
		String name = {};
		bool   versions[128] = {};
		bool   weak[128]     = {};
		u64    sizes[128]    = {};
		isize  chosen_default     = -1;
		isize  chosen_unversioned = -1;

		for (isize inclusion = 0; inclusion < inclusion_count && !r.bad; inclusion++) {
			if (name.text == nullptr) {
				name = cross_abi_cstring(&r);
				gb_zero_array(versions, gb_count_of(versions));
				gb_zero_array(weak,     gb_count_of(weak));
				chosen_default     = -1;
				chosen_unversioned = -1;
			}

			u64 targets = cross_abi_leb128(&r);
			u64 size = 0;
			if (section != Section_Function) {
				size = cross_abi_leb128(&r);
			}
			u8 lib = cross_abi_byte(&r);

			bool is_terminal    = (lib & 0x80) != 0;
			bool is_weak        = false;
			bool is_unversioned = false;
			switch (abi->flavour) {
			case CrossAbi_Glibc:
				lib &= 0x7f;
				break;
			case CrossAbi_FreeBSD:
				is_unversioned = (lib & 0x20) != 0;
				is_weak        = (lib & 0x40) != 0;
				lib &= 0x1f;
				break;
			case CrossAbi_NetBSD:
				is_unversioned = true;
				is_weak        = (lib & 0x40) != 0;
				lib &= 0x1f;
				break;
			}

			bool applies = lib == lib_index && (targets & (1ull << target_index)) != 0;

			for (;;) {
				u8 b = cross_abi_byte(&r);
				isize v = b & 0x7f;
				if (applies && v <= version_index) {
					if (is_unversioned) {
						chosen_unversioned = gb_max(chosen_unversioned, v);
					} else {
						chosen_default = gb_max(chosen_default, v);
						versions[v] = true;
					}
					weak[v]  = is_weak;
					sizes[v] = size;
				}
				if ((b & 0x80) != 0) {
					break;
				}
			}

			if (!is_terminal) {
				continue;
			}

			auto const &emit_contents = [&](String const &symbol, u64 symbol_size) {
				if (section == Section_Function) {
					s = gb_string_append_fmt(s, "%.*s: %s 0\n", LIT(symbol), word);
				} else {
					s = gb_string_append_fmt(s, ".size %.*s, %llu\n%.*s: .fill %llu, 1, 0\n",
						LIT(symbol), cast(unsigned long long)symbol_size,
						LIT(symbol), cast(unsigned long long)symbol_size);
				}
			};

			if (chosen_unversioned >= 0) {
				// .balign 8
				// .globl malloc
				// .type malloc, %function
				// malloc: .quad 0
				s = gb_string_append_fmt(s, ".balign %d\n.%s %.*s\n.type %.*s, %s\n",
					align, weak[chosen_unversioned] ? "weak" : "globl", LIT(name), LIT(name), type);
				emit_contents(name, sizes[chosen_unversioned]);
			}

			for (isize v = 0; v <= version_index; v++) {
				if (!versions[v]) {
					continue;
				}
				// .balign 8
				// .globl _Exit_2_2_5
				// .type _Exit_2_2_5, %function
				// .symver _Exit_2_2_5, _Exit@@GLIBC_2.2.5, remove
				// _Exit_2_2_5: .quad 0
				char symbol_suffix[64] = {};
				char version_name[64]  = {};
				cross_abi_version_name(abi, v, symbol_suffix, gb_size_of(symbol_suffix), true);
				cross_abi_version_name(abi, v, version_name,  gb_size_of(version_name),  false);

				String symbol = cross_fmt("%.*s_%s", LIT(name), symbol_suffix);
				defer (gb_free(heap_allocator(), symbol.text));

				s = gb_string_append_fmt(s, ".balign %d\n.%s %.*s\n.type %.*s, %s\n.symver %.*s, %.*s%s%s, remove\n",
					align, weak[v] ? "weak" : "globl", LIT(symbol), LIT(symbol), type,
					LIT(symbol), LIT(name), v == chosen_default ? "@@" : "@", version_name);
				emit_contents(symbol, sizes[v]);
			}

			name = {};
		}
	}

	if (r.bad) {
		gb_string_free(s);
		return nullptr;
	}
	return s;
}

gb_internal String cross_elf_dynamic_linker(CrossTarget *t) {
	switch (t->libc) {
	case Libc_Glibc:
		switch (t->arch) {
		case TargetArch_amd64:   return str_lit("/lib64/ld-linux-x86-64.so.2");
		case TargetArch_i386:    return str_lit("/lib/ld-linux.so.2");
		case TargetArch_arm64:   return str_lit("/lib/ld-linux-aarch64.so.1");
		case TargetArch_arm32:   return str_lit("/lib/ld-linux-armhf.so.3");
		case TargetArch_riscv64: return str_lit("/lib/ld-linux-riscv64-lp64d.so.1");
		}
		break;
	case Libc_Musl:
		switch (t->arch) {
		case TargetArch_amd64:   return str_lit("/lib/ld-musl-x86_64.so.1");
		case TargetArch_i386:    return str_lit("/lib/ld-musl-i386.so.1");
		case TargetArch_arm64:   return str_lit("/lib/ld-musl-aarch64.so.1");
		case TargetArch_arm32:   return str_lit("/lib/ld-musl-armhf.so.1");
		case TargetArch_riscv64: return str_lit("/lib/ld-musl-riscv64.so.1");
		}
		break;
	case Libc_FreeBSD:
		return str_lit("/libexec/ld-elf.so.1");
	case Libc_NetBSD:
		return str_lit("/libexec/ld.elf_so");
	}
	return {};
}

gb_internal char const *cross_elf_emulation(CrossTarget *t) {
	switch (t->arch) {
	case TargetArch_amd64:   return "elf_x86_64";
	case TargetArch_i386:    return t->os == TargetOs_freebsd ? "elf_i386_fbsd" : "elf_i386";
	case TargetArch_arm64:   return t->os == TargetOs_linux ? "aarch64linux" : "aarch64elf";
	case TargetArch_arm32:   return t->os == TargetOs_linux ? "armelf_linux_eabi" : "armelf";
	case TargetArch_riscv64: return "elf64lriscv";
	}
	return "";
}

struct CrossSharedObjects {
	String        dir;
	Array<String> paths; // in the order they have to be linked in
};

// Builds the stand-ins of the shared libraries of a libc
gb_internal bool cross_build_shared_object_stubs(CrossTarget *t, CrossSharedObjects *out) {
	CrossAbiFlavour flavour = {};
	char const *abilists_path = nullptr;
	String abi_target = {};
	char const *what = "";
	LibcVersion wanted = t->version;

	switch (t->libc) {
	case Libc_Glibc:
		flavour = CrossAbi_Glibc;
		abilists_path = "glibc/abilists";
		what = "glibc";
		switch (t->arch) {
		case TargetArch_arm32: abi_target = str_lit("arm-linux-gnueabihf"); break;
		default:               abi_target = cross_fmt("%s-linux-gnu", cross_arch_name(t->arch)); break;
		}
		break;
	case Libc_FreeBSD:
		flavour = CrossAbi_FreeBSD;
		abilists_path = "freebsd/abilists";
		what = "FreeBSD libc";
		abi_target = cross_fmt("%s-freebsd-none", cross_arch_name(t->arch));
		// FreeBSD 7 is FBSD_1.0, ..., FreeBSD 14 is FBSD_1.7
		wanted = {1, t->version.major - 7, 0};
		break;
	case Libc_NetBSD:
		flavour = CrossAbi_NetBSD;
		abilists_path = "netbsd/abilists";
		what = "NetBSD libc";
		abi_target = cross_fmt("%s-netbsd-none", cross_arch_name(t->arch));
		break;
	default:
		GB_PANIC("no abilists for this libc");
	}

	CrossAbiLists abi = {};
	if (!cross_abilists_load(&abi, flavour, cross_libc_path(abilists_path))) {
		return false;
	}

	isize target_index = -1;
	for_array(i, abi.targets) {
		if (abi.targets[i] == abi_target) {
			target_index = i;
			break;
		}
	}
	if (target_index < 0) {
		gb_printf_err("The bundled %s does not support the target '%.*s'\n", what, LIT(abi_target));
		return false;
	}

	isize version_index = -1;
	for_array(i, abi.versions) {
		int cmp = cross_libc_version_compare(abi.versions[i], wanted);
		if (cmp == 0) {
			version_index = i;
			break;
		}
		if (cmp > 0) {
			break;
		}
	}
	if (version_index < 0) {
		LibcVersion oldest = abi.versions[0];
		LibcVersion newest = abi.versions[abi.versions.count-1];
		if (cross_libc_version_compare(wanted, newest) > 0 && flavour != CrossAbi_Glibc) {
			// A newer release of the OS, the newest known list of symbols has to do
			version_index = abi.versions.count-1;
		} else {
			gb_printf_err("Unknown %s version %.*s\n", what, LIT(cross_libc_version_string(t->version)));
			if (flavour == CrossAbi_Glibc) {
				gb_printf_err("\tThe known versions are:");
				for (LibcVersion const &v : abi.versions) {
					gb_printf_err(" %.*s", LIT(cross_libc_version_string(v)));
				}
				gb_printf_err("\n");
			} else {
				gb_printf_err("\tThe oldest known version is %d.%d, the newest is %d.%d\n", oldest.major, oldest.minor, newest.major, newest.minor);
			}
			return false;
		}
	}

	array_init(&out->paths, heap_allocator());

	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-so", LIT(t->tag)), cross_fmt("the %s %.*s stubs for %s", what, LIT(cross_libc_version_string(t->version)), cross_arch_name(t->arch)), &entry)) {
		return false;
	}
	out->dir = entry.dir;

	bool ok = true;
	CrossJobs compile_jobs = {};
	CrossJobs link_jobs    = {};
	cross_jobs_init(&compile_jobs);
	cross_jobs_init(&link_jobs);

	if (!entry.hit && flavour != CrossAbi_NetBSD) {
		gbString map = gb_string_make(heap_allocator(), "");
		for (isize v = 0; v <= version_index; v++) {
			char version_name[64] = {};
			cross_abi_version_name(&abi, v, version_name, gb_size_of(version_name), false);
			map = gb_string_append_fmt(map, "%s { };\n", version_name);
		}
		ok = cross_write_file(cross_fmt("%.*sall.map", LIT(entry.build_dir)), map, gb_string_length(map));
	}

	String dynamic_linker = cross_elf_dynamic_linker(t);

	for (isize lib_index = 0; lib_index < abi.libs.count && ok; lib_index++) {
		CrossAbiLib const &lib = abi.libs[lib_index];
		if (lib.removed_in.major != 0 && cross_libc_version_compare(t->version, lib.removed_in) >= 0) {
			continue;
		}
		String basename = cross_fmt("lib%s.so.%d", lib.name, lib.sover);
		array_add(&out->paths, cross_fmt("%.*s%.*s", LIT(entry.dir), LIT(basename)));
		if (entry.hit) {
			continue;
		}

		gbString stubs = cross_abilists_generate_stubs(&abi, lib_index, target_index, version_index, t->ptr_size);
		if (stubs == nullptr) {
			gb_printf_err("'%.*s%s' is malformed, the Odin installation is corrupt\n", LIT(cross_libc_root), abilists_path);
			ok = false;
			break;
		}
		String asm_path = cross_fmt("%.*s%s.s", LIT(entry.build_dir), lib.name);
		String obj_path = cross_fmt("%.*s%s.o", LIT(entry.build_dir), lib.name);
		ok = cross_write_file(asm_path, stubs, gb_string_length(stubs));
		gb_string_free(stubs);

		gbString cc = cross_cc_command(t);
		cc = gb_string_append_fmt(cc, " -fPIC -c \"%.*s\" -o \"%.*s\"", LIT(asm_path), LIT(obj_path));
		cross_jobs_add(&compile_jobs, make_string_c(cc));

		// NOTE: the stand-in of the dynamic linker has to have its real name, as that is what
		// ends up in the `DT_NEEDED` entry of the things which get linked against it
		String soname = basename;
		if (gb_strcmp(lib.name, "ld") == 0) {
			soname = filename_without_directory(dynamic_linker);
		}

		gbString ld = gb_string_make(heap_allocator(), "");
		ld = gb_string_append_fmt(ld, "\"%.*s\" -flavor gnu -m %s -shared --hash-style=both -z notext", LIT(cross_tools.lld), cross_elf_emulation(t));
		ld = gb_string_append_fmt(ld, " -soname %.*s", LIT(soname));
		if (flavour != CrossAbi_NetBSD) {
			ld = gb_string_append_fmt(ld, " --version-script \"%.*sall.map\"", LIT(entry.build_dir));
		}
		ld = gb_string_append_fmt(ld, " -o \"%.*s%.*s\" \"%.*s\"", LIT(entry.build_dir), LIT(basename), LIT(obj_path));
		cross_jobs_add(&link_jobs, make_string_c(ld));
	}

	if (!entry.hit) {
		ok = ok && cross_jobs_run(&compile_jobs);
		ok = ok && cross_jobs_run(&link_jobs);
	}
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// compiler-rt

// Builds the "builtins" of compiler-rt: the routines the code generators of LLVM call for the
// operations a target has no instructions for (64-bit division on a 32-bit CPU, arithmetic on
// 128-bit floats, ...). A native build gets them out of the `libgcc` of the system.
gb_internal bool cross_build_compiler_rt(CrossTarget *t, String *library) {
	CrossSourceList list = {};
	if (!cross_source_list_load(&list, cross_libc_path("compiler_rt/sources.txt"))) {
		return false;
	}

	auto sources = array_make<String>(heap_allocator());
	cross_source_list_section(&list, "generic", &sources);
	switch (t->arch) {
	case TargetArch_amd64:   cross_source_list_section(&list, "x86_64",  &sources); break;
	case TargetArch_i386:    cross_source_list_section(&list, "i386",    &sources); break;
	case TargetArch_arm64:   cross_source_list_section(&list, "aarch64", &sources); break;
	case TargetArch_arm32:   cross_source_list_section(&list, "arm",     &sources); break;
	case TargetArch_riscv64: cross_source_list_section(&list, "riscv64", &sources); break;
	}
	switch (t->arch) {
	case TargetArch_amd64: cross_source_list_section(&list, t->os == TargetOs_windows ? "x86_64-windows" : "x86_64-unix", &sources); break;
	case TargetArch_i386:  cross_source_list_section(&list, t->os == TargetOs_windows ? "i386-windows"   : "i386-unix",   &sources); break;
	}

	// The architecture specific implementations take the place of the generic ones
	StringSet overridden = {};
	string_set_init(&overridden, 64);
	defer (string_set_destroy(&overridden));
	for (String const &src : sources) {
		if (string_index_byte(src, '/') >= 0) {
			string_set_add(&overridden, remove_extension_from_path(filename_without_directory(src)));
		}
	}

	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-compiler_rt", LIT(t->rt_tag)), cross_fmt("compiler-rt for %.*s", LIT(t->rt_tag)), &entry)) {
		return false;
	}

	bool is_coff = t->os == TargetOs_windows;
	*library = cross_fmt("%.*s%s", LIT(entry.dir), is_coff ? "compiler_rt.lib" : "compiler_rt.rsp");
	if (entry.hit) {
		return true;
	}

	CrossJobs jobs = {};
	cross_jobs_init(&jobs);
	auto objects = array_make<String>(heap_allocator());

	for (String const &src : sources) {
		if (string_index_byte(src, '/') < 0 && string_set_exists(&overridden, remove_extension_from_path(src))) {
			continue;
		}
		String object = cross_object_name(filename_without_directory(src), is_coff ? ".obj" : ".o");
		array_add(&objects, object);

		gbString cc = cross_cc_command(t);
		cc = gb_string_appendc(cc, " -O2 -fPIC -ffreestanding -fno-builtin -fvisibility=hidden -fomit-frame-pointer -ffunction-sections -fdata-sections");
		if (!cross_is_assembly(src)) {
			cc = gb_string_appendc(cc, " -std=c11");
		}
		cc = gb_string_append_fmt(cc, " -I \"%.*scompiler_rt/builtins\"", LIT(cross_libc_root));
		if (is_coff) {
			cc = gb_string_append_fmt(cc, " -isystem \"%.*sinclude/any-windows-any\"", LIT(cross_libc_root));
		}
		cc = gb_string_append_fmt(cc, " -c \"%.*scompiler_rt/builtins/%.*s\" -o \"%.*s%.*s\"", LIT(cross_libc_root), LIT(src), LIT(entry.build_dir), LIT(object));
		cross_jobs_add(&jobs, make_string_c(cc));
	}

	bool ok = cross_jobs_run(&jobs);
	if (ok) {
		if (is_coff) {
			gbString rsp = gb_string_make(heap_allocator(), "");
			for (String const &object : objects) {
				rsp = gb_string_append_fmt(rsp, "\"%.*s%.*s\"\n", LIT(entry.build_dir), LIT(object));
			}
			String rsp_path = cross_fmt("%.*sobjects.rsp", LIT(entry.build_dir));
			ok = cross_write_file(rsp_path, rsp, gb_string_length(rsp));
			ok = ok && cross_exec(cross_fmt("\"%.*s\" -flavor link /lib /nologo /out:\"%.*scompiler_rt.lib\" \"@%.*s\"",
				LIT(cross_tools.lld), LIT(entry.build_dir), LIT(rsp_path))) == 0;
		} else {
			ok = cross_write_lazy_library(cross_fmt("%.*scompiler_rt.rsp", LIT(entry.build_dir)), entry.dir, objects);
		}
	}
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// glibc

gb_internal char const *cross_glibc_sysdeps_arch(TargetArchKind arch) {
	switch (arch) {
	case TargetArch_amd64:   return "x86_64";
	case TargetArch_i386:    return "i386";
	case TargetArch_arm64:   return "aarch64";
	case TargetArch_arm32:   return "arm";
	case TargetArch_riscv64: return "riscv";
	}
	return "";
}

gb_internal char const *cross_linux_headers_arch(TargetArchKind arch) {
	switch (arch) {
	case TargetArch_amd64:   return "x86";
	case TargetArch_i386:    return "x86";
	case TargetArch_arm64:   return "aarch64";
	case TargetArch_arm32:   return "arm";
	case TargetArch_riscv64: return "riscv";
	}
	return "";
}

gb_internal gbString cross_glibc_include_dirs_arch(gbString s, CrossTarget *t, char const *dir, char const *nptl) {
	String root = cross_libc_root;
	char const *arch = cross_glibc_sysdeps_arch(t->arch);
	bool is_x86 = t->arch == TargetArch_amd64 || t->arch == TargetArch_i386;
	if (nptl != nullptr) {
		s = gb_string_append_fmt(s, " -I \"%.*sglibc/%s/%s/%s\"", LIT(root), dir, arch, nptl);
		if (is_x86) {
			s = gb_string_append_fmt(s, " -I \"%.*sglibc/%s/x86/%s\"", LIT(root), dir, nptl);
		}
	} else {
		s = gb_string_append_fmt(s, " -I \"%.*sglibc/%s/%s\"", LIT(root), dir, arch);
		if (is_x86) {
			s = gb_string_append_fmt(s, " -I \"%.*sglibc/%s/x86\"", LIT(root), dir);
		}
	}
	return s;
}

gb_internal gbString cross_glibc_include_dirs(gbString s, CrossTarget *t) {
	String root = cross_libc_root;
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/include\"", LIT(root));
	s = cross_glibc_include_dirs_arch(s, t, "sysdeps/unix/sysv/linux", nullptr);
	s = cross_glibc_include_dirs_arch(s, t, "sysdeps", "nptl");
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/unix/sysv/linux/generic\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/unix/sysv/linux/include\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/unix/sysv/linux\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/nptl\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/pthread\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/unix/sysv\"", LIT(root));
	s = cross_glibc_include_dirs_arch(s, t, "sysdeps/unix", nullptr);
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/unix\"", LIT(root));
	s = cross_glibc_include_dirs_arch(s, t, "sysdeps", nullptr);
	s = gb_string_append_fmt(s, " -I \"%.*sglibc/sysdeps/generic\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sglibc\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sinclude/%s-linux-gnu\"", LIT(root), cross_linux_headers_arch(t->arch));
	s = gb_string_append_fmt(s, " -I \"%.*sinclude/generic-glibc\"", LIT(root));
	s = gb_string_append_fmt(s, " -I \"%.*sinclude/%s-linux-any\"", LIT(root), cross_linux_headers_arch(t->arch));
	s = gb_string_append_fmt(s, " -I \"%.*sinclude/any-linux-any\"", LIT(root));
	return s;
}

struct CrossCrtFiles {
	String dir;
	String crt1;            // start file of an executable
	String crt1_pie;        // start file of a position independent executable
	String crt1_static_pie;
	String libc_static;     // libc.a
	String libc_shared;     // libc.so
	String libc_nonshared;  // the part of a shared libc which gets linked statically
};

// Builds `Scrt1.o` and `libc_nonshared.a`
gb_internal bool cross_build_glibc_crt(CrossTarget *t, CrossCrtFiles *out) {
	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-crt", LIT(t->tag)), cross_fmt("the glibc %.*s start files for %s", LIT(cross_libc_version_string(t->version)), cross_arch_name(t->arch)), &entry)) {
		return false;
	}
	out->dir            = entry.dir;
	out->crt1           = cross_fmt("%.*sScrt1.o", LIT(entry.dir));
	out->crt1_pie       = out->crt1;
	out->libc_nonshared = cross_fmt("%.*slibc_nonshared.rsp", LIT(entry.dir));
	if (entry.hit) {
		return true;
	}

	String root = cross_libc_root;
	LibcVersion v2_32 = {2, 32, 0};
	LibcVersion v2_33 = {2, 33, 0};
	// libc_nonshared.a redirected the stat functions to xstat until glibc 2.33
	bool nonshared_stat      = cross_libc_version_compare(t->version, v2_32) <= 0;
	// `__libc_start_main` needed statically linked init/fini callbacks until glibc 2.34
	bool start_old_init_fini = cross_libc_version_compare(t->version, v2_33) <= 0;

	gbString base = cross_cc_command(t);
	base = gb_string_append_fmt(base, " -O2 -fPIC -fomit-frame-pointer -D__GLIBC_MINOR__=%d", t->version.minor);

	CrossJobs jobs = {};
	cross_jobs_init(&jobs);

	// Scrt1.o
	{
		gbString cc = gb_string_duplicate(heap_allocator(), base);
		cc = cross_glibc_include_dirs(cc, t);
		cc = gb_string_append_fmt(cc, " -D_LIBC_REENTRANT -include \"%.*sglibc/include/libc-modules.h\" -DMODULE_NAME=libc", LIT(root));
		cc = gb_string_append_fmt(cc, " -Wno-nonportable-include-path -include \"%.*sglibc/include/libc-symbols.h\"", LIT(root));
		cc = gb_string_appendc(cc, " -DPIC -DSHARED -DTOP_NAMESPACE=glibc -DASSEMBLER -Wa,--noexecstack");
		cc = gb_string_append_fmt(cc, " -c \"%.*sglibc/sysdeps/%s/%s\" -o \"%.*sstart.o\"",
			LIT(root), cross_glibc_sysdeps_arch(t->arch), start_old_init_fini ? "start-2.33.S" : "start.S", LIT(entry.build_dir));
		cross_jobs_add(&jobs, make_string_c(cc));
	}
	{
		gbString cc = gb_string_duplicate(heap_allocator(), base);
		cc = gb_string_append_fmt(cc, " -I \"%.*sglibc/csu\"", LIT(root));
		cc = cross_glibc_include_dirs(cc, t);
		cc = gb_string_appendc(cc, " -D_LIBC_REENTRANT -DMODULE_NAME=libc -DTOP_NAMESPACE=glibc -DASSEMBLER -Wa,--noexecstack");
		cc = gb_string_append_fmt(cc, " -c \"%.*sglibc/csu/abi-note.S\" -o \"%.*sabi-note.o\"", LIT(root), LIT(entry.build_dir));
		cross_jobs_add(&jobs, make_string_c(cc));
	}
	{
		gbString cc = gb_string_duplicate(heap_allocator(), base);
		cc = gb_string_append_fmt(cc, " -c \"%.*sglibc/csu/init.c\" -o \"%.*sinit.o\"", LIT(root), LIT(entry.build_dir));
		cross_jobs_add(&jobs, make_string_c(cc));
	}

	// libc_nonshared.a
	struct Dep {
		char const *path;
		bool        include;
	};
	Dep deps[] = {
		{"stdlib/atexit.c",                  true},
		{"stdlib/at_quick_exit.c",           true},
		{"sysdeps/pthread/pthread_atfork.c", true},
		{"debug/stack_chk_fail_local.c",     true},
		{"io/stat-2.32.c",                   nonshared_stat},
		{"io/fstat-2.32.c",                  nonshared_stat},
		{"io/lstat-2.32.c",                  nonshared_stat},
		{"io/stat64-2.32.c",                 nonshared_stat},
		{"io/fstat64-2.32.c",                nonshared_stat},
		{"io/lstat64-2.32.c",                nonshared_stat},
		{"io/fstatat-2.32.c",                nonshared_stat},
		{"io/fstatat64-2.32.c",              nonshared_stat},
		{"io/mknodat-2.32.c",                nonshared_stat},
		{"io/mknod-2.32.c",                  nonshared_stat},
		{"csu/elf-init-2.33.c",              start_old_init_fini},
	};

	auto nonshared_objects = array_make<String>(heap_allocator());
	for (Dep const &dep : deps) {
		if (!dep.include) {
			continue;
		}
		String object = cross_object_name(make_string_c(dep.path), ".o");
		array_add(&nonshared_objects, object);

		gbString cc = gb_string_duplicate(heap_allocator(), base);
		cc = gb_string_appendc(cc, " -std=gnu11 -fgnu89-inline -fmerge-all-constants -frounding-math -Wno-unsupported-floating-point-opt");
		cc = gb_string_appendc(cc, " -fno-common -fmath-errno -ftls-model=initial-exec -Wno-ignored-attributes");
		cc = cross_glibc_include_dirs(cc, t);
		cc = gb_string_appendc(cc, " -DNO_INITFINI");
		if (t->arch == TargetArch_i386) {
			// This prevents i386/sysdep.h from trying to do some silly and unnecessary
			// inline asm hack that uses weird syntax that clang does not support.
			cc = gb_string_appendc(cc, " -DCAN_USE_REGISTER_ASM_EBP");
		}
		cc = gb_string_append_fmt(cc, " -D_LIBC_REENTRANT -include \"%.*sglibc/include/libc-modules.h\" -DMODULE_NAME=libc", LIT(root));
		cc = gb_string_append_fmt(cc, " -Wno-nonportable-include-path -include \"%.*sglibc/include/libc-symbols.h\"", LIT(root));
		cc = gb_string_appendc(cc, " -DPIC -DLIBC_NONSHARED=1 -DTOP_NAMESPACE=glibc");
		cc = gb_string_append_fmt(cc, " -c \"%.*sglibc/%s\" -o \"%.*s%.*s\"", LIT(root), dep.path, LIT(entry.build_dir), LIT(object));
		cross_jobs_add(&jobs, make_string_c(cc));
	}

	bool ok = cross_jobs_run(&jobs);
	ok = ok && cross_exec(cross_fmt("\"%.*s\" -flavor gnu -m %s -r -o \"%.*sScrt1.o\" \"%.*sstart.o\" \"%.*sabi-note.o\" \"%.*sinit.o\"",
		LIT(cross_tools.lld), cross_elf_emulation(t), LIT(entry.build_dir), LIT(entry.build_dir), LIT(entry.build_dir), LIT(entry.build_dir))) == 0;
	ok = ok && cross_write_lazy_library(cross_fmt("%.*slibc_nonshared.rsp", LIT(entry.build_dir)), entry.dir, nonshared_objects);
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// musl

gb_internal char const *cross_musl_arch_name(TargetArchKind arch) {
	switch (arch) {
	case TargetArch_amd64:   return "x86_64";
	case TargetArch_i386:    return "i386";
	case TargetArch_arm64:   return "aarch64";
	case TargetArch_arm32:   return "arm";
	case TargetArch_riscv64: return "riscv64";
	}
	return "";
}

gb_internal bool cross_musl_is_arch_name(String const &name) {
	gb_local_persist char const *names[] = {
		"aarch64", "arm", "generic", "hexagon", "i386", "loongarch64", "m68k", "microblaze", "mips", "mips64",
		"mipsn32", "or1k", "powerpc", "powerpc64", "riscv32", "riscv64", "s390x", "sh", "x32", "x86_64",
	};
	for (char const *n : names) {
		if (name == make_string_c(n)) {
			return true;
		}
	}
	return false;
}

gb_internal gbString cross_musl_cc_command(CrossTarget *t, bool want_O3) {
	String root = cross_libc_root;
	char const *arch = cross_musl_arch_name(t->arch);

	gbString cc = cross_cc_command(t);
	cc = gb_string_appendc(cc, " -std=c99 -ffreestanding -fno-builtin -fexcess-precision=standard -frounding-math -ffp-contract=off");
	cc = gb_string_appendc(cc, " -fno-strict-aliasing -Wa,--noexecstack -D_XOPEN_SOURCE=700");
	cc = gb_string_appendc(cc, " -fomit-frame-pointer -ffunction-sections -fdata-sections -fPIC");
	cc = gb_string_append_fmt(cc, " -I \"%.*smusl/arch/%s\"", LIT(root), arch);
	cc = gb_string_append_fmt(cc, " -I \"%.*smusl/arch/generic\"", LIT(root));
	cc = gb_string_append_fmt(cc, " -I \"%.*smusl/src/include\"", LIT(root));
	cc = gb_string_append_fmt(cc, " -I \"%.*smusl/src/internal\"", LIT(root));
	cc = gb_string_append_fmt(cc, " -I \"%.*smusl/include\"", LIT(root));
	cc = gb_string_append_fmt(cc, " -I \"%.*sinclude/%s-linux-musl\"", LIT(root), t->arch == TargetArch_i386 ? "x86" : arch);
	cc = gb_string_append_fmt(cc, " -I \"%.*sinclude/generic-musl\"", LIT(root));
	cc = gb_string_appendc(cc, want_O3 ? " -O3" : " -Os");
	return cc;
}

gb_internal bool cross_build_musl(CrossTarget *t, bool is_static, CrossCrtFiles *out) {
	String root = cross_libc_root;
	char const *arch = cross_musl_arch_name(t->arch);
	bool time32 = t->arch == TargetArch_i386 || t->arch == TargetArch_arm32;

	// The start files
	{
		CrossCacheEntry entry = {};
		if (!cross_cache_begin(cross_fmt("%.*s-crt", LIT(t->tag)), cross_fmt("the musl start files for %s", cross_arch_name(t->arch)), &entry)) {
			return false;
		}
		out->dir             = entry.dir;
		out->crt1            = cross_fmt("%.*scrt1.o",  LIT(entry.dir));
		out->crt1_pie        = cross_fmt("%.*sScrt1.o", LIT(entry.dir));
		out->crt1_static_pie = cross_fmt("%.*srcrt1.o", LIT(entry.dir));
		if (!entry.hit) {
			CrossJobs jobs = {};
			cross_jobs_init(&jobs);
			char const *names[] = {"crt1", "Scrt1", "rcrt1"};
			for (char const *name : names) {
				gbString cc = cross_musl_cc_command(t, false);
				cc = gb_string_append_fmt(cc, " -DCRT -c \"%.*smusl/crt/%s.c\" -o \"%.*s%s.o\"", LIT(root), name, LIT(entry.build_dir), name);
				cross_jobs_add(&jobs, make_string_c(cc));
			}
			if (!cross_cache_end(&entry, cross_jobs_run(&jobs))) {
				return false;
			}
		}
	}

	if (!is_static) {
		// All a dynamically linked program needs of libc.so are its symbols
		CrossCacheEntry entry = {};
		if (!cross_cache_begin(cross_fmt("%.*s-so", LIT(t->tag)), cross_fmt("the musl libc.so stub for %s", cross_arch_name(t->arch)), &entry)) {
			return false;
		}
		out->libc_shared = cross_fmt("%.*slibc.so", LIT(entry.dir));
		if (entry.hit) {
			return true;
		}
		char const *family = "";
		switch (t->arch) {
		case TargetArch_amd64:
		case TargetArch_i386:    family = "x86";     break;
		case TargetArch_arm64:   family = "aarch64"; break;
		case TargetArch_arm32:   family = "arm";     break;
		case TargetArch_riscv64: family = "riscv";   break;
		}
		gbString cc = cross_cc_command(t);
		cc = gb_string_append_fmt(cc, " -fPIC -DARCH_%s -DFAMILY_%s", arch, family);
		if (t->ptr_size == 8) {
			cc = gb_string_appendc(cc, " -DPTR64");
		} else if (time32) {
			cc = gb_string_appendc(cc, " -DTIME32");
		}
		cc = gb_string_append_fmt(cc, " -c \"%.*smusl/libc.S\" -o \"%.*slibc.o\"", LIT(root), LIT(entry.build_dir));

		bool ok = cross_exec(make_string_c(cc)) == 0;
		ok = ok && cross_exec(cross_fmt("\"%.*s\" -flavor gnu -m %s -shared --hash-style=both -z notext -soname libc.so -o \"%.*slibc.so\" \"%.*slibc.o\"",
			LIT(cross_tools.lld), cross_elf_emulation(t), LIT(entry.build_dir), LIT(entry.build_dir))) == 0;
		return cross_cache_end(&entry, ok);
	}

	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-a", LIT(t->tag)), cross_fmt("musl libc for %s", cross_arch_name(t->arch)), &entry)) {
		return false;
	}
	out->libc_static = cross_fmt("%.*slibc.rsp", LIT(entry.dir));
	if (entry.hit) {
		return true;
	}

	CrossSourceList list = {};
	if (!cross_source_list_load(&list, cross_libc_path("musl/sources.txt"))) {
		cross_cache_end(&entry, false);
		return false;
	}
	auto sources = array_make<String>(heap_allocator());
	cross_source_list_section(&list, "src", &sources);
	cross_source_list_section(&list, "restored", &sources);
	if (time32) {
		cross_source_list_section(&list, "time32", &sources);
	}

	StringSet source_set = {};
	string_set_init(&source_set, 4096);
	defer (string_set_destroy(&source_set));
	for (String const &src : sources) {
		string_set_add(&source_set, src);
	}

	CrossJobs jobs = {};
	cross_jobs_init(&jobs);
	auto objects = array_make<String>(heap_allocator());

	for (String const &src : sources) {
		// When there is a `src/<dir>/<arch>/foo.*` it takes the place of `src/<dir>/foo.*`,
		// even an assembly file can take the place of a C file.
		String dirname      = substring(src, 0, src.len - filename_without_directory(src).len - 1);
		String basename     = remove_extension_from_path(filename_without_directory(src));
		String dir_basename = filename_without_directory(dirname);

		if (cross_musl_is_arch_name(dir_basename)) {
			if (dir_basename != make_string_c(arch)) {
				continue;
			}
		} else {
			bool overridden = false;
			char const *exts[] = {".s", ".S", ".c"};
			for (char const *ext : exts) {
				String candidate = cross_fmt("%.*s/%s/%.*s%s", LIT(dirname), arch, LIT(basename), ext);
				overridden = overridden || string_set_exists(&source_set, candidate);
				gb_free(heap_allocator(), candidate.text);
			}
			if (overridden) {
				continue;
			}
		}

		bool want_O3 = string_ends_with(src, str_lit(".c")) && (
			string_starts_with(src, str_lit("musl/src/malloc/")) ||
			string_starts_with(src, str_lit("musl/src/string/")) ||
			string_starts_with(src, str_lit("musl/src/internal/")));

		String object = cross_object_name(substring(src, 5, src.len), ".o");
		array_add(&objects, object);

		gbString cc = cross_musl_cc_command(t, want_O3);
		cc = gb_string_append_fmt(cc, " -c \"%.*s%.*s\" -o \"%.*s%.*s\"", LIT(root), LIT(src), LIT(entry.build_dir), LIT(object));
		cross_jobs_add(&jobs, make_string_c(cc));
	}

	bool ok = cross_jobs_run(&jobs);
	ok = ok && cross_write_lazy_library(cross_fmt("%.*slibc.rsp", LIT(entry.build_dir)), entry.dir, objects);
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// FreeBSD and NetBSD

// Builds the start files, which are all put into one object
gb_internal bool cross_build_bsd_crt(CrossTarget *t, CrossCrtFiles *out) {
	bool is_freebsd = t->libc == Libc_FreeBSD;
	char const *os_name = is_freebsd ? "FreeBSD" : "NetBSD";

	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-crt", LIT(t->tag)), cross_fmt("the %s start files for %s", os_name, cross_arch_name(t->arch)), &entry)) {
		return false;
	}
	out->dir      = entry.dir;
	out->crt1     = cross_fmt("%.*scrt1.o", LIT(entry.dir));
	out->crt1_pie = out->crt1;
	if (entry.hit) {
		return true;
	}

	String root = cross_libc_root;
	char const *arch = cross_arch_name(t->arch);

	struct Source {
		String path;
		bool   is_asm;
	};
	auto sources = array_make<Source>(heap_allocator());
	gbString cflags  = gb_string_make(heap_allocator(), "");
	gbString acflags = gb_string_make(heap_allocator(), "");
	gbString include = gb_string_make(heap_allocator(), "");
	String csu = {};

	if (is_freebsd) {
		char const *csu_arch = "";
		switch (t->arch) {
		case TargetArch_amd64: csu_arch = "amd64";   break;
		case TargetArch_i386:  csu_arch = "i386";    break;
		case TargetArch_arm64: csu_arch = "aarch64"; break;
		}
		csu = cross_fmt("%.*sfreebsd/lib/csu/", LIT(root));

		cflags  = gb_string_appendc(cflags, " -O2 -fno-common -std=gnu99");
		acflags = gb_string_append_fmt(acflags, " -DLOCORE -D__FreeBSD_version=%d", t->version.major*100000 + 500);
		include = gb_string_appendc(include, " -DPIC -DSTRIP_FBSDID");
		include = gb_string_append_fmt(include, " -I \"%.*sinclude/%s-freebsd-none\"", LIT(root), arch);
		include = gb_string_append_fmt(include, " -I \"%.*sinclude/generic-freebsd\"", LIT(root));
		include = gb_string_append_fmt(include, " -I \"%.*s%s\"", LIT(csu), csu_arch);
		include = gb_string_append_fmt(include, " -I \"%.*scommon\"", LIT(csu));
		include = gb_string_append_fmt(include, " -I \"%.*sfreebsd/lib/libc/include\"", LIT(root));

		array_add(&sources, Source{str_lit("common/crtbegin.c"),         false});
		array_add(&sources, Source{str_lit("common/crtbrand.S"),         true});
		array_add(&sources, Source{str_lit("common/crtend.c"),           false});
		array_add(&sources, Source{str_lit("common/feature_note.S"),     true});
		array_add(&sources, Source{str_lit("common/ignore_init_note.S"), true});
		array_add(&sources, Source{cross_fmt("%s/crt1_c.c", csu_arch),   false});
		array_add(&sources, Source{cross_fmt("%s/crt1_s.S", csu_arch),   true});
	} else {
		csu = cross_fmt("%.*snetbsd/lib/csu/", LIT(root));

		cflags  = gb_string_appendc(cflags, " -O2 -DHAVE_INITFINI_ARRAY");
		acflags = gb_string_append_fmt(acflags, " -D__NetBSD_Version__=%d", t->version.major*100000000 + t->version.minor*1000000);
		include = gb_string_append_fmt(include, " -I \"%.*sinclude/%s-netbsd-none\"", LIT(root), arch);
		include = gb_string_append_fmt(include, " -I \"%.*sinclude/generic-netbsd\"", LIT(root));
		include = gb_string_append_fmt(include, " -I \"%.*scommon\"", LIT(csu));

		array_add(&sources, Source{str_lit("common/crt0-common.c"),      false});
		array_add(&sources, Source{str_lit("common/crtbegin.c"),         false});
		array_add(&sources, Source{str_lit("common/sysident.S"),         true});
		array_add(&sources, Source{cross_fmt("arch/%s/crt0.S", arch),    true});
	}

	CrossJobs jobs = {};
	cross_jobs_init(&jobs);
	gbString ld = gb_string_make(heap_allocator(), "");
	ld = gb_string_append_fmt(ld, "\"%.*s\" -flavor gnu -m %s -r -o \"%.*scrt1.o\"", LIT(cross_tools.lld), cross_elf_emulation(t), LIT(entry.build_dir));

	for (Source const &src : sources) {
		String object = cross_object_name(src.path, ".o");
		gbString cc = cross_cc_command(t);
		cc = gb_string_append_fmt(cc, " -fPIC -fno-omit-frame-pointer%s%s", src.is_asm ? acflags : cflags, include);
		cc = gb_string_append_fmt(cc, " -c \"%.*s%.*s\" -o \"%.*s%.*s\"", LIT(csu), LIT(src.path), LIT(entry.build_dir), LIT(object));
		cross_jobs_add(&jobs, make_string_c(cc));
		ld = gb_string_append_fmt(ld, " \"%.*s%.*s\"", LIT(entry.build_dir), LIT(object));
	}

	bool ok = cross_jobs_run(&jobs);
	ok = ok && cross_exec(make_string_c(ld)) == 0;
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// mingw-w64

struct CrossMingwFiles {
	String crt2;        // start file of an executable
	String dllcrt2;     // start file of a DLL
	String libmingw32;
};

gb_internal gbString cross_mingw_cc_command(CrossTarget *t, bool crt) {
	String root = cross_libc_root;
	gbString cc = cross_cc_command(t);
	cc = gb_string_appendc(cc, " -O2 -std=gnu11 -D__USE_MINGW_ANSI_STDIO=0 -D__MSVCRT_VERSION__=0xE00 -D_WIN32_WINNT=0x0a00");
	if (t->arch != TargetArch_i386) {
		// The old 32-bit x86 variant of SEH doesn't use tables.
		cc = gb_string_appendc(cc, " -fasynchronous-unwind-tables");
	}
	cc = gb_string_append_fmt(cc, " -isystem \"%.*sinclude/any-windows-any\"", LIT(root));
	if (crt) {
		// According to Martin Storsjö, the files under mingw-w64-crt are designed to always
		// be built with __MSVCRT_VERSION__=0x700
		cc = gb_string_appendc(cc, " -U__MSVCRT_VERSION__ -D__MSVCRT_VERSION__=0x700 -D_CRTBLD -D_SYSCRT=1 -U_WIN32_WINNT -D_WIN32_WINNT=0x0f00 -DCRTDLL=1 -DHAVE_CONFIG_H");
		cc = gb_string_append_fmt(cc, " -I \"%.*smingw/include\"", LIT(root));
	}
	return cc;
}

gb_internal bool cross_build_mingw_crt(CrossTarget *t, CrossMingwFiles *out) {
	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-crt", LIT(t->tag)), cross_fmt("the mingw-w64 runtime for %s", cross_arch_name(t->arch)), &entry)) {
		return false;
	}
	out->crt2       = cross_fmt("%.*scrt2.obj",      LIT(entry.dir));
	out->dllcrt2    = cross_fmt("%.*sdllcrt2.obj",   LIT(entry.dir));
	out->libmingw32 = cross_fmt("%.*slibmingw32.lib", LIT(entry.dir));
	if (entry.hit) {
		return true;
	}

	String root = cross_libc_root;
	CrossSourceList list = {};
	if (!cross_source_list_load(&list, cross_libc_path("mingw/sources.txt"))) {
		cross_cache_end(&entry, false);
		return false;
	}

	CrossJobs jobs = {};
	cross_jobs_init(&jobs);

	{
		gbString cc = cross_mingw_cc_command(t, true);
		cc = gb_string_append_fmt(cc, " -c \"%.*smingw/crt/crtexe.c\" -o \"%.*scrt2.obj\"", LIT(root), LIT(entry.build_dir));
		cross_jobs_add(&jobs, make_string_c(cc));
	}
	{
		gbString cc = cross_mingw_cc_command(t, true);
		cc = gb_string_append_fmt(cc, " -c \"%.*smingw/crt/crtdll.c\" -o \"%.*sdllcrt2.obj\"", LIT(root), LIT(entry.build_dir));
		cross_jobs_add(&jobs, make_string_c(cc));
	}

	auto crt_sources = array_make<String>(heap_allocator());
	cross_source_list_section(&list, "generic", &crt_sources);
	cross_source_list_section(&list, "x86", &crt_sources);
	if (t->arch == TargetArch_i386) {
		cross_source_list_section(&list, "x86_32", &crt_sources);
	}
	auto winpthreads_sources = array_make<String>(heap_allocator());
	cross_source_list_section(&list, "winpthreads", &winpthreads_sources);

	gbString rsp = gb_string_make(heap_allocator(), "");
	for (isize pass = 0; pass < 2; pass++) {
		Array<String> const &sources = pass == 0 ? crt_sources : winpthreads_sources;
		for (String const &src : sources) {
			String object = cross_object_name(src, ".obj");
			gbString cc = cross_mingw_cc_command(t, pass == 0);
			if (pass == 1) {
				// winpthreads incorrectly assumes that Clang has `-Wprio-ctor-dtor`.
				cc = gb_string_appendc(cc, " -DIN_WINPTHREAD -Wno-unknown-warning-option");
			}
			cc = gb_string_append_fmt(cc, " -c \"%.*smingw/%.*s\" -o \"%.*s%.*s\"", LIT(root), LIT(src), LIT(entry.build_dir), LIT(object));
			cross_jobs_add(&jobs, make_string_c(cc));
			rsp = gb_string_append_fmt(rsp, "\"%.*s%.*s\"\n", LIT(entry.build_dir), LIT(object));
		}
	}

	bool ok = cross_jobs_run(&jobs);
	String rsp_path = cross_fmt("%.*sobjects.rsp", LIT(entry.build_dir));
	ok = ok && cross_write_file(rsp_path, rsp, gb_string_length(rsp));
	ok = ok && cross_exec(cross_fmt("\"%.*s\" -flavor link /lib /nologo /out:\"%.*slibmingw32.lib\" \"@%.*s\"",
		LIT(cross_tools.lld), LIT(entry.build_dir), LIT(rsp_path))) == 0;
	return cross_cache_end(&entry, ok);
}

gb_internal String cross_mingw_find_def(CrossTarget *t, String const &name) {
	char const *arch_dir = t->arch == TargetArch_i386 ? "lib32" : "lib64";
	String candidates[] = {
		cross_fmt("%.*smingw/%s/%.*s.def",            LIT(cross_libc_root), arch_dir, LIT(name)),
		cross_fmt("%.*smingw/%s/%.*s.def.in",         LIT(cross_libc_root), arch_dir, LIT(name)),
		cross_fmt("%.*smingw/lib-common/%.*s.def",    LIT(cross_libc_root), LIT(name)),
		cross_fmt("%.*smingw/lib-common/%.*s.def.in", LIT(cross_libc_root), LIT(name)),
	};
	for (String const &candidate : candidates) {
		if (cross_file_exists(candidate)) {
			return candidate;
		}
	}
	return {};
}

// Makes the import library of a DLL of Windows out of its `.def` file.
// `library` is left empty if there is no `.def` file for it.
gb_internal bool cross_build_mingw_import_library(CrossTarget *t, String const &name, String *library) {
	*library = {};
	String def = cross_mingw_find_def(t, name);
	if (def.len == 0) {
		return true;
	}

	CrossCacheEntry entry = {};
	if (!cross_cache_begin(cross_fmt("%.*s-implib-%.*s", LIT(t->tag), LIT(name)), str_lit(""), &entry)) {
		return false;
	}
	*library = cross_fmt("%.*s%.*s.lib", LIT(entry.dir), LIT(name));
	if (entry.hit) {
		return true;
	}

	// The `.def` files use the C preprocessor to tell the architectures apart
	String processed = cross_fmt("%.*s%.*s.def", LIT(entry.build_dir), LIT(name));
	gbString cc = cross_cc_command(t);
	cc = gb_string_append_fmt(cc, " -E -P -x c -I \"%.*smingw/def-include\" \"%.*s\" -o \"%.*s\"", LIT(cross_libc_root), LIT(def), LIT(processed));
	bool ok = cross_exec(make_string_c(cc)) == 0;

	if (ok) {
		String out = cross_fmt("%.*s%.*s.lib", LIT(entry.build_dir), LIT(name));
		String names[] = {str_lit("llvm-dlltool"), cross_fmt("llvm-dlltool-%d", LLVM_VERSION_MAJOR)};
		String dlltool = cross_find_in_path(names, gb_count_of(names));
		if (dlltool.len != 0) {
			ok = cross_exec(cross_fmt("\"%.*s\" -m %s%s -d \"%.*s\" -l \"%.*s\"", LIT(dlltool),
				t->arch == TargetArch_i386 ? "i386" : "i386:x86-64",
				t->arch == TargetArch_i386 ? " -k" : "",
				LIT(processed), LIT(out))) == 0;
		} else {
			ok = cross_exec(cross_fmt("\"%.*s\" -flavor link /lib /nologo /machine:%s /def:\"%.*s\" /out:\"%.*s\"", LIT(cross_tools.lld),
				t->arch == TargetArch_i386 ? "x86" : "x64",
				LIT(processed), LIT(out))) == 0;
		}
	}
	return cross_cache_end(&entry, ok);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Linking

// Libraries which are a part of the libc, or of the compiler runtime, on at least one of the
// platforms. They are always linked, there is never a need to look for them.
gb_internal bool cross_is_libc_library(CrossTarget *t, String const &name) {
	gb_local_persist char const *unix_names[] = {
		"c", "m", "pthread", "dl", "rt", "util", "resolv", "crypt", "xnet", "thr", "stdthreads", "execinfo",
		"gcc", "gcc_s", "gcc_eh", "System", "System.framework",
	};
	gb_local_persist char const *windows_names[] = {
		"libcmt", "libcmtd", "msvcrt", "msvcrtd", "libucrt", "libucrtd", "ucrt", "ucrtd",
		"libvcruntime", "libvcruntimed", "vcruntime", "vcruntimed", "legacy_stdio_definitions", "oldnames",
		"mingw32", "mingwex",
	};
	if (t->os == TargetOs_windows) {
		for (char const *n : windows_names) {
			if (str_eq_ignore_case(name, make_string_c(n))) {
				return true;
			}
		}
		return false;
	}
	for (char const *n : unix_names) {
		if (name == make_string_c(n)) {
			return true;
		}
	}
	return false;
}

gb_internal String cross_quote(String const &arg) {
	if (arg.len >= 2 && arg[0] == '"') {
		return arg;
	}
	return cross_fmt("\"%.*s\"", LIT(arg));
}

gb_internal void cross_tokenize_flags(String const &flags, Array<String> *out) {
	isize i = 0;
	while (i < flags.len) {
		while (i < flags.len && gb_char_is_space(flags[i])) i++;
		if (i >= flags.len) {
			break;
		}
		gbString token = gb_string_make(heap_allocator(), "");
		bool in_quotes = false;
		for (; i < flags.len; i++) {
			u8 c = flags[i];
			if (c == '"') {
				in_quotes = !in_quotes;
				continue;
			}
			if (!in_quotes && gb_char_is_space(c)) {
				break;
			}
			token = gb_string_append_length(token, &c, 1);
		}
		array_add(out, make_string_c(token));
	}
}

// The extra linker flags of a program are written for the compiler driver (`clang`), which is
// what a native build links with. The few of them that are meant for it and not for the linker
// are translated here.
gb_internal void cross_translate_linker_flags(CrossTarget *t, String const &flags, Array<String> *args) {
	auto tokens = array_make<String>(heap_allocator());
	cross_tokenize_flags(flags, &tokens);

	if (t->os == TargetOs_windows) {
		for (String const &token : tokens) {
			array_add(args, cross_quote(token));
		}
		return;
	}

	for (isize i = 0; i < tokens.count; i++) {
		String token = tokens[i];
		if (string_starts_with(token, str_lit("-Wl,"))) {
			String_Iterator it = {substring(token, 4, token.len), 0};
			String part = {};
			while (string_split_iterator_next(&it, ',', &part)) {
				if (part.len != 0) {
					array_add(args, cross_quote(part));
				}
			}
		} else if (token == "-Xlinker") {
			if (i+1 < tokens.count) {
				array_add(args, cross_quote(tokens[++i]));
			}
		} else if (token == "-pthread" || token == "-nostdlib" || token == "-nodefaultlibs" || token == "-nostartfiles") {
			// nothing to do
		} else if (token == "-rdynamic") {
			array_add(args, str_lit("--export-dynamic"));
		} else if (string_starts_with(token, str_lit("-l")) && token.len > 2 && cross_is_libc_library(t, substring(token, 2, token.len))) {
			// always linked
		} else if (token == "-framework" || token == "-weak_framework") {
			array_add(args, token);
			if (i+1 < tokens.count) {
				array_add(args, cross_quote(tokens[++i]));
			}
		} else {
			array_add(args, cross_quote(token));
		}
	}
}

// Assembles a foreign import of an assembly file
gb_internal bool cross_assemble(CrossTarget *t, String const &asm_file, String *object) {
	String temp_dir = temporary_directory(temporary_allocator());
	bool is_coff = t->os == TargetOs_windows;
	String name = cross_fmt("%.*s-%llx%s", LIT(filename_without_directory(asm_file)),
		cast(unsigned long long)fnv64a(asm_file.text, asm_file.len), is_coff ? ".obj" : ".o");
	if (temp_dir.len != 0) {
		*object = cross_fmt("%.*s/%.*s", LIT(temp_dir), LIT(name));
	} else {
		*object = cross_fmt("%.*s%s", LIT(asm_file), is_coff ? ".obj" : ".o");
	}

	bool is_x86 = t->arch == TargetArch_amd64 || t->arch == TargetArch_i386;
	bool gas_syntax = string_ends_with(asm_file, str_lit(".s")) || string_ends_with(asm_file, str_lit(".S"));
	if (is_x86 && !gas_syntax) {
		char const *format = "";
		if (t->os == TargetOs_windows) {
			format = t->ptr_size == 8 ? "win64" : "win32";
		} else if (t->os == TargetOs_darwin) {
			format = t->ptr_size == 8 ? "macho64" : "macho32";
		} else {
			format = t->ptr_size == 8 ? "elf64" : "elf32";
		}
		char const *nasm = gb_get_env("ODIN_NASM_PATH", permanent_allocator());
		if (nasm == nullptr) {
			nasm = "nasm";
		}
		i32 result = cross_exec(cross_fmt("\"%s\" \"%.*s\" -f %s -o \"%.*s\" %.*s", nasm, LIT(asm_file), format, LIT(*object), LIT(build_context.extra_assembler_flags)));
		if (result != 0) {
			gb_printf_err("executing `nasm` to assemble foreign import of %.*s failed.\n\tSuggestion: `nasm` does not ship with the compiler and should be installed with your system's package manager.\n", LIT(asm_file));
			return false;
		}
		return true;
	}

	if (!cross_find_cc()) {
		return false;
	}
	gbString cc = gb_string_make(heap_allocator(), "");
	cc = gb_string_append_fmt(cc, "\"%.*s\" --target=%.*s %.*s -c \"%.*s\" -o \"%.*s\" %.*s",
		LIT(cross_tools.cc), LIT(t->cc_triple), LIT(t->cc_flags), LIT(asm_file), LIT(*object), LIT(build_context.extra_assembler_flags));
	return cross_exec(make_string_c(cc)) == 0;
}

struct CrossLinkInputs {
	Array<String> objects;
	Array<String> libraries;  // arguments for the linker, in the order of the foreign imports
	Array<String> flags;      // the `extra_linker_flags` of the foreign imports
	Array<String> dlls;       // names of the DLLs for which an import library is needed
};

gb_internal bool cross_collect_link_inputs(LinkerData *gen, CrossTarget *t, CrossLinkInputs *in) {
	array_init(&in->objects,   heap_allocator());
	array_init(&in->libraries, heap_allocator());
	array_init(&in->flags,     heap_allocator());
	array_init(&in->dlls,      heap_allocator());

	StringSet seen = {};
	string_set_init(&seen, 64);
	defer (string_set_destroy(&seen));

	bool is_windows = t->os == TargetOs_windows;
	bool is_darwin  = t->os == TargetOs_darwin;

	auto asm_objects = array_make<String>(heap_allocator());

	for (Entity *e : gen->foreign_libraries) {
		GB_ASSERT(e->kind == Entity_LibraryName);
		String extra_linker_flags = string_trim_whitespace(e->LibraryName.extra_linker_flags);
		if (extra_linker_flags.len != 0) {
			cross_translate_linker_flags(t, extra_linker_flags, &in->flags);
		}

		for (String lib : e->LibraryName.paths) {
			lib = string_trim_whitespace(lib);
			if (lib.len == 0) {
				continue;
			}

			if (has_asm_extension(lib)) {
				if (string_set_update(&seen, lib)) {
					continue;
				}
				String object = {};
				if (!cross_assemble(t, lib, &object)) {
					return false;
				}
				array_add(&asm_objects, object);
				continue;
			}

			// NOTE: the order and the repetitions of static libraries matter to the linker, shared ones are only ever needed once
			bool is_static = string_ends_with(lib, str_lit(".a")) || string_ends_with(lib, str_lit(".o")) || string_ends_with(lib, str_lit(".obj"));
			if (string_set_update(&seen, lib) && (build_context.min_link_libs || !is_static || e->LibraryName.ignore_duplicates || is_windows)) {
				continue;
			}

			bool is_path = string_index_byte(lib, '/') >= 0 || string_index_byte(lib, '\\') >= 0;

			if (is_windows) {
				if (is_path) {
					array_add(&in->libraries, cross_quote(lib));
					continue;
				}
				String name = copy_string(heap_allocator(), lib);
				string_to_lower(&name);
				if (string_ends_with(name, str_lit(".lib"))) {
					name = substring(name, 0, name.len-4);
				} else if (string_ends_with(name, str_lit(".dll"))) {
					name = substring(name, 0, name.len-4);
				}
				if (cross_is_libc_library(t, name)) {
					continue;
				}
				if (cross_mingw_find_def(t, name).len != 0) {
					array_add(&in->dlls, name);
				} else {
					array_add(&in->libraries, cross_quote(lib));
				}
				continue;
			}

			if (is_darwin && string_ends_with(lib, str_lit(".framework"))) {
				String name = remove_extension_from_path(filename_without_directory(lib));
				if (name == "System") {
					continue;
				}
				array_add(&in->libraries, str_lit("-framework"));
				array_add(&in->libraries, cross_quote(name));
				continue;
			}

			if (cross_is_libc_library(t, lib)) {
				continue;
			}

			bool is_file = is_static || string_ends_with(lib, str_lit(".so")) || string_contains_string(lib, str_lit(".so.")) ||
			               string_ends_with(lib, str_lit(".dylib")) || string_ends_with(lib, str_lit(".tbd"));
			if (is_path) {
				array_add(&in->libraries, cross_quote(lib));
			} else if (is_file && !is_darwin) {
				array_add(&in->libraries, cross_fmt("\"-l:%.*s\"", LIT(lib)));
			} else {
				array_add(&in->libraries, cross_fmt("\"-l%.*s\"", LIT(lib)));
			}
		}
	}

	for (String const &object : asm_objects) {
		array_add(&in->objects, object);
	}
	for (String const &object : gen->output_object_paths) {
		array_add(&in->objects, object);
	}
	return true;
}

// Runs LLD with its arguments in a response file, as there can be thousands of them
gb_internal i32 cross_run_linker(char const *flavor, Array<String> const &args, String const &output_filename, char const *mode = "") {
	gbString rsp = gb_string_make_reserve(heap_allocator(), 4096);
	defer (gb_string_free(rsp));
	for (String const &arg : args) {
		rsp = gb_string_append_fmt(rsp, "%.*s\n", LIT(arg));
	}

	if (build_context.print_linker_flags) {
		for (String const &arg : args) {
			gb_printf("%.*s ", LIT(arg));
		}
		gb_printf("\n");
		return 0;
	}

	String rsp_path = cross_fmt("%.*s.link.rsp", LIT(output_filename));
	if (!cross_write_file(rsp_path, rsp, gb_string_length(rsp))) {
		return 1;
	}
	if (build_context.show_system_calls) {
		gb_printf_err("[SYSTEM CALL] %.*s -flavor %s %s", LIT(cross_tools.lld), flavor, mode);
		for (String const &arg : args) {
			gb_printf_err(" %.*s", LIT(arg));
		}
		gb_printf_err("\n");
	}
	bool show_system_calls = build_context.show_system_calls;
	build_context.show_system_calls = false;
	i32 result = cross_exec(cross_fmt("\"%.*s\" -flavor %s %s \"@%.*s\"", LIT(cross_tools.lld), flavor, mode, LIT(rsp_path)));
	build_context.show_system_calls = show_system_calls;

	if (!build_context.keep_temp_files) {
		gb_file_remove(cast(char const *)rsp_path.text);
	}
	return result;
}

gb_internal i32 cross_link_elf(LinkerData *gen, CrossTarget *t, String const &output_filename) {
	BuildContext *bc = &build_context;
	bool is_exe    = bc->build_mode == BuildMode_Executable;
	bool is_static = bc->link_mode == LinkMode_Static;
	bool is_pie    = is_exe && bc->reloc_mode == RelocMode_PIC;
	bool link_libc = !bc->no_crt;

	CrossCrtFiles      crt = {};
	CrossSharedObjects shared_objects = {};
	String             compiler_rt = {};

	if (link_libc) {
		switch (t->libc) {
		case Libc_Glibc:
			// NOTE: the stubs come first as that is where an unknown version gets reported
			if (!cross_build_shared_object_stubs(t, &shared_objects)) return 1;
			if (!cross_build_glibc_crt(t, &crt))                      return 1;
			break;
		case Libc_Musl:
			if (!cross_build_musl(t, is_static, &crt))              return 1;
			break;
		case Libc_FreeBSD:
		case Libc_NetBSD:
			if (!cross_build_shared_object_stubs(t, &shared_objects)) return 1;
			if (!cross_build_bsd_crt(t, &crt))                        return 1;
			break;
		}
	}
	if (!cross_build_compiler_rt(t, &compiler_rt)) {
		return 1;
	}

	CrossLinkInputs in = {};
	if (!cross_collect_link_inputs(gen, t, &in)) {
		return 1;
	}

	auto args = array_make<String>(heap_allocator(), 0, 256);
	array_add(&args, str_lit("-m"));
	array_add(&args, make_string_c(cross_elf_emulation(t)));
	array_add(&args, str_lit("--eh-frame-hdr"));
	array_add(&args, str_lit("--hash-style=both"));
	array_add(&args, str_lit("--build-id"));

	switch (t->os) {
	case TargetOs_linux:
		// required for RELRO
		array_add(&args, str_lit("-z"));
		array_add(&args, str_lit("now"));
		array_add(&args, str_lit("-z"));
		array_add(&args, str_lit("relro"));
		break;
	case TargetOs_netbsd:
		// NetBSD expects 2 PT_LOAD segments in a shared object, otherwise ld.elf_so fails to load it
		array_add(&args, str_lit("--no-rosegment"));
		array_add(&args, str_lit("-z"));
		array_add(&args, str_lit("norelro"));
		break;
	}

	if (bc->lto_kind != LTO_None) {
		array_add(&args, cross_fmt("--thinlto-jobs=%d", cast(int)bc->thread_count));
	}

	if (bc->build_mode == BuildMode_DynamicLibrary) {
		array_add(&args, str_lit("-shared"));
		// NOTE(dweiler): _odin_entry_point must be called at initialization
		// time of the shared object, similarly, _odin_exit_point must be called
		// at deinitialization.
		array_add(&args, str_lit("-init"));
		array_add(&args, str_lit("_odin_entry_point"));
		array_add(&args, str_lit("-fini"));
		array_add(&args, str_lit("_odin_exit_point"));
	} else if (is_static && link_libc) {
		array_add(&args, str_lit("-static"));
		if (is_pie) {
			// A static position independent executable, the start files of musl relocate it
			array_add(&args, str_lit("-pie"));
			array_add(&args, str_lit("--no-dynamic-linker"));
			array_add(&args, str_lit("-z"));
			array_add(&args, str_lit("text"));
		}
	} else {
		// NOTE: without a libc there is nothing in a position independent executable which
		// could relocate it, that is left to the dynamic linker as with a native build
		if (is_pie) {
			array_add(&args, str_lit("-pie"));
		}
		if (link_libc || is_pie) {
			array_add(&args, str_lit("--dynamic-linker"));
			array_add(&args, cross_elf_dynamic_linker(t));
		}
	}

	array_add(&args, str_lit("-o"));
	array_add(&args, cross_quote(output_filename));

	if (is_exe && link_libc) {
		String crt1 = crt.crt1;
		if (is_pie) {
			crt1 = is_static ? crt.crt1_static_pie : crt.crt1_pie;
		}
		array_add(&args, cross_quote(crt1));
	}

	for (String const &object : in.objects) {
		array_add(&args, cross_quote(object));
	}

	if (!bc->no_rpath) {
		// Set the rpath to the path of the executable, so that dynamic libraries are looked for at that path.
		array_add(&args, str_lit("-rpath"));
		array_add(&args, str_lit("$ORIGIN"));
	}

	for (String const &flag : in.flags) {
		array_add(&args, flag);
	}
	for (String const &library : in.libraries) {
		array_add(&args, library);
	}
	cross_translate_linker_flags(t, bc->extra_linker_flags, &args);

	if (link_libc) {
		if (shared_objects.paths.count != 0) {
			array_add(&args, str_lit("--as-needed"));
			for (String const &path : shared_objects.paths) {
				array_add(&args, cross_quote(path));
			}
			array_add(&args, str_lit("--no-as-needed"));
		}
		if (crt.libc_nonshared.len != 0) {
			array_add(&args, cross_fmt("\"@%.*s\"", LIT(crt.libc_nonshared)));
		}
		if (crt.libc_static.len != 0) {
			array_add(&args, cross_fmt("\"@%.*s\"", LIT(crt.libc_static)));
		}
		if (crt.libc_shared.len != 0) {
			array_add(&args, cross_quote(crt.libc_shared));
		}
	}

	// NOTE: compiler-rt comes after the libc as the libc relies on it too. It is given twice
	// around a static libc as some of its routines call into the libc in turn (`abort`).
	array_add(&args, cross_fmt("\"@%.*s\"", LIT(compiler_rt)));
	if (link_libc && crt.libc_static.len != 0) {
		array_add(&args, cross_fmt("\"@%.*s\"", LIT(crt.libc_static)));
	}

	return cross_run_linker("gnu", args, output_filename);
}

gb_internal i32 cross_link_macho(LinkerData *gen, CrossTarget *t, String const &output_filename) {
	BuildContext *bc = &build_context;

	CrossLinkInputs in = {};
	if (!cross_collect_link_inputs(gen, t, &in)) {
		return 1;
	}

	// NOTE: only libSystem is bundled, the frameworks are a part of the SDK of Apple which cannot
	// be redistributed. If there is one at hand it is used.
	String sdk = cross_env("SDKROOT");

	bool needs_sdk = false;
	for (String const &library : in.libraries) {
		if (library == "-framework") {
			needs_sdk = true;
		}
	}
	if (needs_sdk && sdk.len == 0) {
		gb_printf_err("The program links against frameworks of macOS:");
		for_array(i, in.libraries) {
			if (in.libraries[i] == "-framework" && i+1 < in.libraries.count) {
				String name = in.libraries[i+1];
				if (name.len >= 2 && name[0] == '"') {
					name = substring(name, 1, name.len-1);
				}
				gb_printf_err(" %.*s", LIT(name));
			}
		}
		gb_printf_err("\n");
		gb_printf_err("\tOnly libSystem is bundled with the compiler, frameworks are a part of the macOS SDK.\n");
		gb_printf_err("\tSuggestion: set the SDKROOT environment variable to the path of a macOS SDK\n");
		return 1;
	}

	String min_version = bc->minimum_os_version_string;
	String sdk_version = min_version;
	LibcVersion v = {};
	if (parse_libc_version(min_version, &v) && v.major < 14) {
		sdk_version = str_lit("14.0.0");
	}

	auto args = array_make<String>(heap_allocator(), 0, 256);
	array_add(&args, str_lit("-arch"));
	array_add(&args, t->arch == TargetArch_arm64 ? str_lit("arm64") : str_lit("x86_64"));
	array_add(&args, str_lit("-platform_version"));
	array_add(&args, str_lit("macos"));
	array_add(&args, min_version);
	array_add(&args, sdk_version);
	if (sdk.len != 0) {
		array_add(&args, str_lit("-syslibroot"));
		array_add(&args, cross_quote(sdk));
	}

	String init_object = {};
	if (bc->build_mode == BuildMode_DynamicLibrary) {
		array_add(&args, str_lit("-dylib"));
		array_add(&args, str_lit("-install_name"));
		array_add(&args, cross_fmt("\"@rpath/%.*s\"", LIT(filename_without_directory(output_filename))));

		// NOTE(dweiler): _odin_entry_point must be called at initialization time of the shared object.
		// LLD has no `-init` for Mach-O, an initializer which the dynamic linker runs does the same.
		String init_asm = cross_fmt("%.*s.init.s", LIT(output_filename));
		char const *init_source =
			"\t.section __DATA,__mod_init_func,mod_init_funcs\n"
			"\t.p2align 3\n"
			"\t.quad __odin_entry_point\n";
		if (!cross_write_file(init_asm, init_source, gb_strlen(init_source))) {
			return 1;
		}
		bool ok = cross_assemble(t, init_asm, &init_object);
		gb_file_remove(cast(char const *)init_asm.text);
		if (!ok) {
			return 1;
		}
	}
	if (bc->lto_kind != LTO_None) {
		array_add(&args, cross_fmt("--thinlto-jobs=%d", cast(int)bc->thread_count));
	}

	array_add(&args, str_lit("-o"));
	array_add(&args, cross_quote(output_filename));
	if (init_object.len != 0) {
		array_add(&args, cross_quote(init_object));
	}

	for (String const &object : in.objects) {
		array_add(&args, cross_quote(object));
	}

	if (!bc->no_rpath) {
		array_add(&args, str_lit("-rpath"));
		array_add(&args, str_lit("@loader_path"));
	}

	for (String const &flag : in.flags) {
		array_add(&args, flag);
	}
	for (String const &library : in.libraries) {
		array_add(&args, library);
	}
	cross_translate_linker_flags(t, bc->extra_linker_flags, &args);

	if (!bc->no_crt) {
		if (sdk.len == 0) {
			array_add(&args, cross_fmt("\"-L%.*sdarwin\"", LIT(cross_libc_root)));
		}
		array_add(&args, str_lit("-lSystem"));
	}

	i32 result = cross_run_linker("darwin", args, output_filename);
	if (result != 0) {
		return result;
	}

	if (bc->ODIN_DEBUG && !bc->print_linker_flags) {
		// NOTE: macOS does not link the debug information, it stays in the object files, which
		// are about to be deleted. `dsymutil` collects it into a `.dSYM` bundle.
		String names[] = {str_lit("dsymutil"), str_lit("llvm-dsymutil"), cross_fmt("dsymutil-%d", LLVM_VERSION_MAJOR)};
		String dsymutil = cross_find_in_path(names, gb_count_of(names));
		if (dsymutil.len != 0) {
			result = cross_exec(cross_fmt("\"%.*s\" \"%.*s\"", LIT(dsymutil), LIT(output_filename)));
		} else {
			gb_printf_err("Warning: `dsymutil` was not found, the program will not have any debug information\n");
		}
	}
	return result;
}

gb_internal i32 cross_link_coff(LinkerData *gen, CrossTarget *t, String const &output_filename) {
	BuildContext *bc = &build_context;
	bool is_dll    = bc->build_mode == BuildMode_DynamicLibrary;
	bool is_i386   = t->arch == TargetArch_i386;
	bool link_libc = !bc->no_crt;

	CrossMingwFiles mingw = {};
	String compiler_rt = {};
	if (link_libc && !cross_build_mingw_crt(t, &mingw)) {
		return 1;
	}
	if (!cross_build_compiler_rt(t, &compiler_rt)) {
		return 1;
	}

	CrossLinkInputs in = {};
	if (!cross_collect_link_inputs(gen, t, &in)) {
		return 1;
	}

	// The DLLs the runtime itself relies on
	CrossSourceList list = {};
	if (!cross_source_list_load(&list, cross_libc_path("mingw/sources.txt"))) {
		return 1;
	}
	if (link_libc) {
		cross_source_list_section(&list, "always_link", &in.dlls);
	} else {
		array_add(&in.dlls, str_lit("kernel32"));
		array_add(&in.dlls, str_lit("ntdll"));
	}

	auto args = array_make<String>(heap_allocator(), 0, 256);
	array_add(&args, str_lit("-nologo"));
	array_add(&args, str_lit("-lldmingw"));
	array_add(&args, is_i386 ? str_lit("-machine:x86") : str_lit("-machine:x64"));
	array_add(&args, str_lit("-incremental:no"));
	array_add(&args, str_lit("-opt:ref"));
	array_add(&args, cross_fmt("-subsystem:%.*s", LIT(windows_subsystem_names[bc->ODIN_WINDOWS_SUBSYSTEM])));

	if (bc->ODIN_DEBUG) {
		array_add(&args, str_lit("-debug"));
		array_add(&args, str_lit("-opt:noicf"));
		if (bc->build_paths[BuildPath_Symbols].name != "") {
			String symbol_path = path_to_string(heap_allocator(), bc->build_paths[BuildPath_Symbols]);
			array_add(&args, cross_fmt("\"-pdb:%.*s\"", LIT(symbol_path)));
		}
	}
	if (bc->lto_kind != LTO_None) {
		array_add(&args, cross_fmt("-opt:lldltojobs=%d", cast(int)bc->thread_count));
	}

	if (is_dll) {
		array_add(&args, str_lit("-dll"));
		if (!link_libc) {
			array_add(&args, str_lit("-noentry"));
		} else if (is_i386) {
			array_add(&args, str_lit("-entry:DllMainCRTStartup@12"));
		} else {
			array_add(&args, str_lit("-entry:DllMainCRTStartup"));
		}
	} else {
		array_add(&args, str_lit("-entry:mainCRTStartup"));
	}
	if (!link_libc) {
		array_add(&args, str_lit("-nodefaultlib"));
	}

	array_add(&args, cross_fmt("\"-out:%.*s\"", LIT(output_filename)));

	if (link_libc) {
		array_add(&args, is_i386 ? str_lit("-alternatename:__image_base__=___ImageBase") : str_lit("-alternatename:__image_base__=__ImageBase"));
		array_add(&args, cross_quote(is_dll ? mingw.dllcrt2 : mingw.crt2));
	}

	for (String const &object : in.objects) {
		array_add(&args, cross_quote(object));
	}

	if (bc->has_resource) {
		// NOTE: a `.rc` file needs the resource compiler and the headers of the Windows SDK, only precompiled resources can be linked
		if (bc->build_paths[BuildPath_RC].basename != "") {
			gb_printf_err("Resource files (.rc) cannot be compiled without the Windows SDK, use a precompiled .res file with -resource\n");
			return 1;
		}
		array_add(&args, quote_path(heap_allocator(), bc->build_paths[BuildPath_RES]));
	}

	for (String const &flag : in.flags) {
		array_add(&args, flag);
	}
	cross_translate_linker_flags(t, bc->extra_linker_flags, &args);
	for (String const &library : in.libraries) {
		array_add(&args, library);
	}

	if (link_libc) {
		array_add(&args, cross_quote(mingw.libmingw32));
	}
	array_add(&args, cross_quote(compiler_rt));

	StringSet seen = {};
	string_set_init(&seen, 64);
	defer (string_set_destroy(&seen));
	for (String const &dll : in.dlls) {
		if (string_set_update(&seen, dll)) {
			continue;
		}
		String library = {};
		if (!cross_build_mingw_import_library(t, dll, &library)) {
			return 1;
		}
		if (library.len != 0) {
			array_add(&args, cross_quote(library));
		}
	}
	if (link_libc) {
		// NOTE: again, for what the import libraries of the CRT pull out of it
		array_add(&args, cross_quote(mingw.libmingw32));
		array_add(&args, cross_quote(compiler_rt));
	}

	return cross_run_linker("link", args, output_filename);
}

gb_internal i32 cross_link_static_library(CrossTarget *t, LinkerData *gen, String const &output_filename) {
	if (t->os == TargetOs_windows) {
		auto args = array_make<String>(heap_allocator(), 0, 64);
		array_add(&args, str_lit("-nologo"));
		array_add(&args, cross_fmt("\"-out:%.*s\"", LIT(output_filename)));
		for (String const &object : gen->output_object_paths) {
			array_add(&args, cross_quote(object));
		}
		// NOTE: LLD only acts as the librarian when `/lib` is the very first of its arguments
		return cross_run_linker("link", args, output_filename, "/lib");
	}

	// NOTE: `llvm-ar` knows the object formats of every platform, the `ar` of the system might only know its own
	String names[] = {str_lit("llvm-ar"), cross_fmt("llvm-ar-%d", LLVM_VERSION_MAJOR), str_lit("ar")};
	String ar = cross_find_in_path(names, gb_count_of(names));
	if (ar.len == 0) {
		gb_printf_err("Could not find `llvm-ar` or `ar`, which is needed to create a static library\n");
		return 1;
	}
	char const *c = cross_cstr(output_filename);
	gb_file_remove(c);

	gbString cmd = gb_string_make(heap_allocator(), "");
	cmd = gb_string_append_fmt(cmd, "\"%.*s\" rcs \"%.*s\"", LIT(ar), LIT(output_filename));
	for (String const &object : gen->output_object_paths) {
		cmd = gb_string_append_fmt(cmd, " \"%.*s\"", LIT(object));
	}
	return cross_exec(make_string_c(cmd));
}

// Links the program against the libc which is bundled with the compiler
gb_internal i32 cross_linker_stage(LinkerData *gen, String const &output_filename) {
	BuildContext *bc = &build_context;
	Timings *timings = &global_timings;

	cross_libc_root = concatenate_strings(permanent_allocator(), bc->ODIN_ROOT, str_lit("libc/"));
	cross_libc_root = normalize_path(permanent_allocator(), cross_libc_root, NIX_SEPARATOR_STRING);
	if (!path_is_directory(cross_libc_root)) {
		gb_printf_err("The directory of the bundled C libraries is missing: %.*s\n", LIT(cross_libc_root));
		gb_printf_err("\tSuggestion: use -libc:system to link with the toolchain of the system\n");
		return 1;
	}

	CrossTarget target = {};
	if (!cross_target_init(&target)) {
		gb_printf_err("There is no bundled libc for the target %.*s_%.*s\n", LIT(target_os_names[bc->metrics.os]), LIT(target_arch_names[bc->metrics.arch]));
		return 1;
	}

	if (bc->build_mode == BuildMode_StaticLibrary) {
		timings_start_section(timings, str_lit("Static Library Creation"));
		if (target.os == TargetOs_windows && !cross_find_lld()) {
			return 1;
		}
		return cross_link_static_library(&target, gen, output_filename);
	}

	if (!cross_find_lld()) {
		return 1;
	}
	// NOTE: nothing has to be built for macOS, all there is of its libc is a list of symbols
	if (target.libc != Libc_Darwin && !cross_find_cc()) {
		return 1;
	}

	timings_start_section(timings, str_lit("lld-link"));
	switch (target.libc) {
	case Libc_Mingw:
		return cross_link_coff(gen, &target, output_filename);
	case Libc_Darwin:
		return cross_link_macho(gen, &target, output_filename);
	default:
		return cross_link_elf(gen, &target, output_filename);
	}
}
