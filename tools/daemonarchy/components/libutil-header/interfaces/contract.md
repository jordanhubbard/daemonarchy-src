# Header interface

Expose the exact original `libutil.h`, including `expand_number(const char *,
int64_t *)`, `expand_unsigned(const char *, uint64_t *)` and the original C
generic macro dispatch. Preserve declarations outside the selected parser API;
their presence does not imply their implementations are included in this unit.
Consumers bind the same header identity, applicable native types and target ABI.
