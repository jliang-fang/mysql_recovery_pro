def create_page_header(page_type):
    """Create a minimal InnoDB page header"""
    page = bytearray(PAGE_SIZE)
    
    # FIL Header (38 bytes)
    write_le_uint32(page, 0x00, page_type)      # Page type
    write_le_uint32(page, 0x04, 0)              # Checksum1
    write_le_uint32(page, 0x08, 0)              # LSN (64-bit low)
    write_le_uint32(page, 0x0C, 0)              # LSN high
    write_le_uint16(page, 0x10, 0)              # Page offset
    write_le_uint16(page, 0x12, 0xFFFF)         # Previous page (none)
    write_le_uint16(page, 0x14, 0xFFFF)         # Next page (none)
    write_le_uint32(page, 0x18, 0)              # B-tree index (0 = leaf)
    
    return page

def create_sdi_page(table_name):
    """Create a SDI page with compressed JSON"""
    schema = {
        "schema": {"name": "test_recovery"},
        "table": {
            "name": table_name,
            "columns": [
                {"name": "id", "type": MYSQL_TYPE_LONGLONG, "is_nullable": False},
                {"name": "name", "type": MYSQL_TYPE_VARCHAR, "is_nullable": False, 
                 "length": 128, "charset_id": 255, "collation_id": 255,
                 "default_value": "unknown"},
                {"name": "email", "type": MYSQL_TYPE_VARCHAR, "is_nullable": True,
                 "length": 255, "charset_id": 255, "collation_id": 255},
                {"name": "created_at", "type": MYSQL_TYPE_DATETIME, "is_nullable": True,
                 "default_value": "CURRENT_TIMESTAMP"},
                {"name": "metadata", "type": MYSQL_TYPE_JSON, "is_nullable": True}
            ],
            "indexes": [
                {"name": "PRIMARY", "columns": ["id"], "is_primary": True},
                {"name": "idx_name", "columns": ["name"], "is_unique": False}
            ]
        }
    }
    
    json_str = json.dumps(schema, separators=(',', ':'))
    compressed = zlib.compress(json_str.encode(), 1)[2:]  # Skip zlib header
    
    sdi_header = bytearray(20)
    write_le_uint32(sdi_header, 0, 1)           # type
    write_le_uint64(sdi_header, 4, 1)           # id
    write_le_uint32(sdi_header, 12, len(json_str))  # uncompressed_len
    write_le_uint32(sdi_header, 16, len(compressed)) # compressed_len
    
    payload = sdi_header + compressed
    page = create_page_header(FIL_PAGE_SDI)
    
    # Write SDI payload at offset 38 (after FIL header)
    page[38:38+len(payload)] = payload
    
    return bytes(page)

def generate_ibd(table_name="users"):
    """Generate a minimal .ibd file with SDI + empty B+ tree"""
    pages = []
    
    # Page 0: SDI
    pages.append(create_sdi_page(table_name))
    
    # Page 1: Root page of clustered index (empty)
    root_page = create_page_header(FIL_PAGE_INDEX)
    write_le_uint32(root_page, 38, 0)   # PAGE_LEVEL = 0 (leaf)
    write_le_uint16(root_page, 42, 2)   # PAGE_N_HEAP = 2 (infimum + supremum)
    
    pages.append(bytes(root_page))
    
    return b''.join(pages)
