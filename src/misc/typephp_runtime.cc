#include <phpx.h>
#include <typephp_runtime.h>
#include <typephp_opcode_table.h>

BEGIN_EXTERN_C()
#include "ext/standard/basic_functions.h"
#include "sapi/embed/php_embed.h"
#if !defined(PHP_WIN32) && !defined(__wasi__) && !defined(PHPX_IOS) && !defined(PHPX_ANDROID)
#include "ps_title.h"
#endif
END_EXTERN_C()

static php_stream *s_in_process = nullptr;

static void cli_register_file_handles() {
    php_stream *s_in = php_stream_open_wrapper_ex("php://stdin", "rb", 0, nullptr, nullptr);
    php_stream *s_out = php_stream_open_wrapper_ex("php://stdout", "wb", 0, nullptr, nullptr);
    php_stream *s_err = php_stream_open_wrapper_ex("php://stderr", "wb", 0, nullptr, nullptr);

    /* Release stream resources, but don't free the underlying handles. Otherwise,
     * extensions which write during mshutdown/gshutdown lose the standard streams. */
    if (s_in) s_in->flags |= PHP_STREAM_FLAG_NO_RSCR_DTOR_CLOSE;
    if (s_out) s_out->flags |= PHP_STREAM_FLAG_NO_RSCR_DTOR_CLOSE;
    if (s_err) s_err->flags |= PHP_STREAM_FLAG_NO_RSCR_DTOR_CLOSE;

    if (s_in == nullptr || s_out == nullptr || s_err == nullptr) {
        if (s_in) php_stream_close(s_in);
        if (s_out) php_stream_close(s_out);
        if (s_err) php_stream_close(s_err);
        return;
    }

    s_in_process = s_in;
    zend_constant ic, oc, ec;
    php_stream_to_zval(s_in, &ic.value);
    php_stream_to_zval(s_out, &oc.value);
    php_stream_to_zval(s_err, &ec.value);

    Z_CONSTANT_FLAGS(ic.value) = 0;
    ic.name = zend_string_init_interned("STDIN", sizeof("STDIN") - 1, 0);
    zend_register_constant(&ic);

    Z_CONSTANT_FLAGS(oc.value) = 0;
    oc.name = zend_string_init_interned("STDOUT", sizeof("STDOUT") - 1, 0);
    zend_register_constant(&oc);

    Z_CONSTANT_FLAGS(ec.value) = 0;
    ec.name = zend_string_init_interned("STDERR", sizeof("STDERR") - 1, 0);
    zend_register_constant(&ec);
}

// Register the generated module during MINIT, before Zend snapshots persistent
// symbols and collects request handlers. Late MODULE_TEMPORARY registration
// leaves Swoole-created requests without the compiled functions and RINIT.
static zend_module_entry *typephp_runtime_module = nullptr;
static decltype(zend_module_entry::request_startup_func) project_request_startup = nullptr;
static decltype(zend_module_entry::request_shutdown_func) project_request_shutdown = nullptr;
static decltype(php_embed_module.startup) embed_startup = nullptr;
static bool typephp_runtime_started = false;
static bool defer_main_request = true;

static zend_result runtime_request_startup(INIT_FUNC_ARGS) {
#ifdef ZTS
    const bool main_thread = tsrm_is_main_thread();
#else
    const bool main_thread = true;
#endif
    // Keep application startup after php_embed_init(), when the host has
    // installed its standard streams and embedded-file hooks. Workers use
    // Zend's ordinary RINIT path, including their own request-local caches.
    if (main_thread && defer_main_request) {
        return SUCCESS;
    }
    typephp_opcode_table_request_startup();
    return project_request_startup ? project_request_startup(INIT_FUNC_ARGS_PASSTHRU) : SUCCESS;
}

static zend_result runtime_request_shutdown(SHUTDOWN_FUNC_ARGS) {
    const auto result = project_request_shutdown ? project_request_shutdown(SHUTDOWN_FUNC_ARGS_PASSTHRU) : SUCCESS;
    typephp_opcode_table_request_shutdown();
    return result;
}

static int runtime_module_startup(sapi_module_struct *sapi) {
    return php_module_startup(sapi, typephp_runtime_module);
}

extern "C" int typephp_runtime_start(typephp_module_getter get_module,
                                     typephp_module_pre_shutdown pre_shutdown,
                                     int argc,
                                     char **argv) {
    if (typephp_runtime_started) {
        return 0;
    }
    // Kept in the host ABI for older generated entry files. Permanent modules
    // release their class-owned data through the normal MSHUTDOWN callback.
    (void) pre_shutdown;
    typephp_runtime_module = get_module();
    project_request_startup = typephp_runtime_module->request_startup_func;
    project_request_shutdown = typephp_runtime_module->request_shutdown_func;
    typephp_runtime_module->request_startup_func = runtime_request_startup;
    typephp_runtime_module->request_shutdown_func = runtime_request_shutdown;
    embed_startup = php_embed_module.startup;
    php_embed_module.startup = runtime_module_startup;
    defer_main_request = true;
    if (php_embed_init(argc, argv) != SUCCESS) {
        php_embed_module.startup = embed_startup;
        typephp_runtime_module->request_startup_func = project_request_startup;
        typephp_runtime_module->request_shutdown_func = project_request_shutdown;
        typephp_runtime_module = nullptr;
        return 255;
    }
    // Even a failed application entry must shut down the active PHP request.
    typephp_runtime_started = true;
    defer_main_request = false;
    typephp_opcode_table_install();

#if !defined(PHP_WIN32) && !defined(__wasi__) && !defined(PHPX_IOS) && !defined(PHPX_ANDROID)
    save_ps_args(argc, argv);
#endif

    int rc = 0;
    zend_first_try {
        try {
            static char path_translated[] = "embed";
            cli_register_file_handles();
            SG(request_info).path_translated = path_translated;
            if (runtime_request_startup(typephp_runtime_module->type, typephp_runtime_module->module_number) !=
                SUCCESS) {
                rc = 255;
                zend_error(E_ERROR, "Failed to initialize request for module [%s]", typephp_runtime_module->name);
            }
        } catch (zend_object *e) {
            if (!zend_is_graceful_exit(e)) {
                CG(unclean_shutdown) = 1;
                zend_exception_error(e, E_ERROR);
            }
            rc = EG(exit_status);
        }
    }
    zend_end_try();
    return rc != 0 ? rc : EG(exit_status);
}

extern "C" void typephp_runtime_stop(void) {
    if (!typephp_runtime_started) {
        return;
    }
    // Zend now owns RSHUTDOWN/MSHUTDOWN, symbol removal and object teardown.
    // In particular, never unload a process module from a worker request.
    php_embed_shutdown();
    typephp_opcode_table_uninstall();
    php_embed_module.startup = embed_startup;
    typephp_runtime_module->request_startup_func = project_request_startup;
    typephp_runtime_module->request_shutdown_func = project_request_shutdown;
    typephp_runtime_module = nullptr;
    project_request_startup = nullptr;
    project_request_shutdown = nullptr;
    typephp_runtime_started = false;
}
