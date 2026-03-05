[[nodiscard]] bool is_overflow() const { return (offset & 0x80000000) != 0; }
