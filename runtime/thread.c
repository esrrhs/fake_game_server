/* The thread runtime's cross-translation-unit state — FakeCC dialect.
 *
 * The implementation is per-OS: thread_linux.c drives __clone with a
 * hand-built stack and TLS image, thread_darwin.c drives bsdthread_*.
 * Selected by filename (see c_file_matches_target in src/pkg.c).
 *
 * What lives here is only what has to be found by name across translation
 * units.  The linker builds a PT_TLS segment from these, and each platform
 * file reads them to lay out a thread's TLS, so they are defined once here
 * rather than repeated -- a second definition would be a duplicate symbol.
 *
 * Types are per-file in this dialect: a package does not share struct
 * definitions between its files, so each platform file carries its own.
 */
package runtime;

void *__fakecc_tls_image = 0;
unsigned long __fakecc_tls_filesz = 0;
unsigned long __fakecc_tls_memsz = 0;
unsigned long __fakecc_tls_align = 16;
