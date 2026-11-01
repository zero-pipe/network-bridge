# Error Codes

Public error codes are defined in [`include/nb/error.h`](../../include/nb/error.h).
`NB_ERROR_TABLE` is the single source of truth: it generates the enum values,
stable names, descriptions and retry flags. The `0..-9` values already used by
earlier releases are unchanged.

## Layout

| Value | Category |
| --- | --- |
| `0` | success |
| `-1..-9` | original framework codes, kept compatible |
| `-10..-19` | extended framework codes |
| `<= -1000` | application-defined codes |

## Framework codes

| Name | Value | Meaning | Retryable |
| --- | ---: | --- | :---: |
| `NB_OK` | 0 | success | no |
| `NB_ERR_INVALID` | -1 | invalid argument | no |
| `NB_ERR_NOMEM` | -2 | out of memory | no |
| `NB_ERR_NOTFOUND` | -3 | unknown service, method, route or request | no |
| `NB_ERR_TIMEOUT` | -4 | operation timed out | no |
| `NB_ERR_IO` | -5 | I/O error | no |
| `NB_ERR_STATE` | -6 | invalid object state | no |
| `NB_ERR_AGAIN` | -7 | temporarily unavailable, retry later | yes |
| `NB_ERR_TOOBIG` | -8 | message or field exceeds its limit | no |
| `NB_ERR_CLOSED` | -9 | object or connection closed | no |
| `NB_ERR_ALREADY` | -10 | already started or entry exists | no |
| `NB_ERR_NOT_SUPPORTED` | -11 | unsupported by this build or transport | no |
| `NB_ERR_CANCELED` | -12 | operation canceled | no |
| `NB_ERR_OVERLOAD` | -13 | server overloaded, retry later | yes |
| `NB_ERR_BAD_MESSAGE` | -14 | malformed message or payload | no |
| `NB_ERR_PERMISSION` | -15 | insufficient permission | no |
| `NB_ERR_ADDR_IN_USE` | -16 | address already in use | no |
| `NB_ERR_CONN_REFUSED` | -17 | connection refused | no |
| `NB_ERR_CONN_RESET` | -18 | connection reset by peer | no |
| `NB_ERR_BUSY` | -19 | resource busy, retry later | yes |
| `NB_ERR_USER_BASE` | -1000 | first application-defined value | no |

`NB_ERR_TIMEOUT` is deliberately not marked retryable: a retry may re-execute a
non-idempotent operation. Callers should combine business idempotency with the
reason for the timeout before retrying.

## Application codes

Application status codes travel through `nb_response_t.status` and
`nb_reply(call, status, ...)`. Applications should use negative values below
`NB_ERR_USER_BASE`, or reserve their own non-negative range, so they cannot
collide with framework codes:

```c
enum {
    APP_ERR_ORDER_NOT_FOUND = -1001,
    APP_ERR_OUT_OF_STOCK = -1002
};
```

A client callback receives both the framework `status` and the application
status in `resp->status`. Do not conflate them.

## Query helpers

```c
#include <nb/error.h>

int rc = nb_proxy_call_sync(...);
if (rc != NB_OK) {
    nb_log_error("call failed: %s (%d), retry=%d",
                 nb_error_name(rc), rc, nb_error_is_retryable(rc));
}
```

`nb_error_name()` returns the stable symbolic name, for example
`NB_ERR_TIMEOUT`; `nb_strerror()` returns a human-readable description;
`nb_error_is_retryable()` returns non-zero when the caller may retry. Unknown
values never return `NULL`.

## Adding a code

Add an entry to `NB_ERROR_TABLE`; the enum and lookup tables are generated.
New codes must use unused values and existing numeric values must never change,
because callers, log statistics and the wire status field depend on them.
