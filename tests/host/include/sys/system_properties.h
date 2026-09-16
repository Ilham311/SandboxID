#pragma once

/* Host shim for <sys/system_properties.h>.
 *
 * The other Android-only header module_impl.hpp pulls in. run.sh reaches for
 * this only when the real one is missing.
 *
 * prop_info is the whole story as a type, and the module never lays one out,
 * copies one, or dereferences one: the hook TU holds it strictly by pointer (it
 * comes back from the real __system_property_find at runtime, on-device, inside
 * a hooked function whose body the host suite never executes). Pointers to an
 * incomplete type are all that is needed there.
 *
 * The two bounds constants are used to clamp buffers, so they need real values.
 * These are bionic's, unchanged since Android 5.0 — the same ones Termux's own
 * header ships. The __system_property_* entry points are never called on the
 * host; the module hooks them by name (as string literals), so they are
 * deliberately not declared here — an accidental host call should fail to
 * compile, not silently resolve to a stub. */

typedef struct prop_info prop_info;

#define PROP_VALUE_MAX 92
#define PROP_NAME_MAX  32
