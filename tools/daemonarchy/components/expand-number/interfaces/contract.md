# Number expansion interface

Retain the original signed `expand_number` and unsigned `expand_unsigned`
interfaces declared by the shared header. Successful calls return zero and the
parsed 64-bit result; invalid syntax and out-of-range input return minus one
with the original `EINVAL`/`ERANGE` behavior. Preserve optional signs, native
base parsing, case-insensitive binary suffixes and the original generic dispatch.
No other libutil API is supplied by this slice.
