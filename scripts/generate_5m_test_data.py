#!/usr/bin/env python3
# =============================================================================
# Generate 5M+ Test Data for MySQL Recovery Testing
# =============================================================================

import subprocess
import time
import random
import string
import tempfile
import os

def generate_username(n=10):
    """Generate a random username"""
    return 'user_' + ''.join(random.choices(string.ascii_lowercase + string.digits, k=n))

def generate_email(username):
    """Generate email from username"""
    domains = ['@gmail.com', '@yahoo.com', '@hotmail.com', '@outlook.com', '@example.com']
    return username + random.choice(domains)

def generate_full_name():
    """Generate a random full name"""
    first_names = ['John', 'Jane', 'Michael', 'Sarah', 'David', 'Emily', 'Chris', 'Lisa', 'Tom', 'Mary']
    last_names = ['Smith', 'Johnson', 'Williams', 'Brown', 'Jones', 'Garcia', 'Miller', 'Davis', 'Rodriguez', 'Martinez']
    return f"{random.choice(first_names)} {random.choice(last_names)}"

def main():
    # Number of records
    NUM_RECORDS = 5500000
    BATCH_SIZE = 50000
    
    print(f"Generating {NUM_RECORDS:,} test records...")
    print(f"Batch size: {BATCH_SIZE:,}")
    
    start_time = time.time()
    
    # Generate SQL file in batches
    for batch_num in range(NUM_RECORDS // BATCH_SIZE):
        batch_start = batch_num * BATCH_SIZE
        batch_end = batch_start + BATCH_SIZE
        
        sql_lines = ["INSERT INTO recovery_test.test_users (username, email, full_name) VALUES"]
        
        for i in range(batch_start, batch_end):
            username = generate_username()
            email = generate_email(username)
            full_name = generate_full_name()
            
            # Escape single quotes
            username_escaped = username.replace("'", "\\'")
            full_name_escaped = full_name.replace("'", "\\'")
            
            sql_lines.append(f"('{username_escaped}', '{email}', '{full_name}')")
            
            if i < batch_end - 1:
                sql_lines[-1] += ","
        
        sql_content = "\n".join(sql_lines) + ";\n"
        
        # Write to temp file and execute
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql_content)
            sql_file = f.name
        
        try:
            result = subprocess.run(
                ["mysql", "<", sql_file],
                shell=True,
                capture_output=True,
                text=True
            )
            
            if result.returncode != 0:
                print(f"Error on batch {batch_num}: {result.stderr}")
                return False
        finally:
            os.unlink(sql_file)
        
        if (batch_num + 1) % 50 == 0:
            elapsed = time.time() - start_time
            rate = (batch_num + 1) * BATCH_SIZE / elapsed
            print(f"Batch {batch_num + 1}/{NUM_RECORDS // BATCH_SIZE} | "
                  f"Records: {(batch_num + 1) * BATCH_SIZE:,} | "
                  f"Rate: {rate:.0f} records/sec | "
                  f"Elapsed: {elapsed/60:.1f}min")
    
    # Insert remaining records
    remaining = NUM_RECORDS % BATCH_SIZE
    if remaining > 0:
        sql_lines = ["INSERT INTO recovery_test.test_users (username, email, full_name) VALUES"]
        for i in range(remaining):
            username = generate_username()
            email = generate_email(username)
            full_name = generate_full_name()
            
            username_escaped = username.replace("'", "\\'")
            full_name_escaped = full_name.replace("'", "\\'")
            
            sql_lines.append(f"('{username_escaped}', '{email}', '{full_name}')")
            
            if i < remaining - 1:
                sql_lines[-1] += ","
        
        sql_content = "\n".join(sql_lines) + ";\n"
        
        with tempfile.NamedTemporaryFile(mode='w', suffix='.sql', delete=False) as f:
            f.write(sql_content)
            sql_file = f.name
        
        try:
            subprocess.run(["mysql", "<", sql_file], shell=True)
        finally:
            os.unlink(sql_file)
    
    elapsed = time.time() - start_time
    print(f"\nGenerated {NUM_RECORDS:,} records in {elapsed/60:.1f} minutes")
    
    # Verify count
    result = subprocess.run(
        ["mysql", "-e", "SELECT COUNT(*) FROM recovery_test.test_users"],
        capture_output=True,
        text=True
    )
    print(f"Verification count: {result.stdout.strip()}")
    
    # Get table info
    result = subprocess.run(
        ["mysql", "-e", "SHOW TABLE STATUS FROM recovery_test LIKE 'test_users'"],
        capture_output=True,
        text=True
    )
    print(f"\nTable info:\n{result.stdout}")
    
    return True

if __name__ == "__main__":
    success = main()
    exit(0 if success else 1)
