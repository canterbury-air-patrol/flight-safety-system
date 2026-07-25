# 43 — `secure_string` limits long-lived resident copies, not every transient

**Decided** 2026-07-10. Shipped in 1.2.0. The scope statement also lives at the
top of `src/secure-string.hpp`, where anyone reaching for the type will read it.

## Context

`secure_string` scrubs its storage on destruction. Once such a type exists there
is a standing temptation to chase every `std::string` a credential ever touched,
which in this codebase means the wire I/O buffers and the parsed `Json::Value`
configuration — neither of which can be scrubbed without invasive changes (a
wiping allocator for `std::string`, and for jsoncpp).

## Decision

The goal is limiting **long-lived resident copies** of credentials — cache and
member fields such as `smm_settings`' username/password and
`db_connection::pass_` — not eliminating every transient copy a credential
passes through on its way there.

Explicitly out of scope: wire I/O buffers and the parsed `Json::Value` config.
They are freed promptly, and scrubbing them buys defence-in-depth only against
an adversary who can already read process memory.

Two cheap exceptions are closed directly rather than left inconsistent:

- the recv-side frame for `message_type_smm_settings` is wiped right after
  decode (`buf_len::wipeSecure()`, from `fss_connection::recvMsg`);
- the packed `buf_len` for an outgoing `smm_settings` message is wiped right
  after the send attempt (`fss_connection::sendMsg`).

## Implementation trap: the ECPG accessor returns by value

`toNulTerminated()` returns a `std::string` **by value**, not a cached pointer
into the `secure_string`. `pass_` is shared by the read and the write database
connections, which reconnect concurrently from separate threads, so a cached
mutable scratch buffer races. The first implementation did cache, and it was
caught by a heap-corruption abort during `make check` — not by review.

## Related

`secure_string::operator==` is constant-time (todo/57): both overloads check
length first (length is not the secret) then accumulate
`volatile unsigned char diff |= a[i] ^ b[i]` across the full content, so a
mismatch anywhere costs the same to detect. No production caller compares
secrets today; the property exists so that the first one to try it is safe.
