// insert_test_data.cpp - 批量插入测试数据
#include <mysql/mysql.h>
#include <iostream>
#include <string>
#include <ctime>

const int BATCH_SIZE = 100000;
const int TOTAL_RECORDS = 5200000;

std::string gen_email(int id) {
    return "user_" + std::string(8 - std::to_string(id).length(), '0') + std::to_string(id) + "@test.com";
}

std::string gen_data(int id) {
    return "data_" + std::to_string(id * 12345 + 67890);
}

int main() {
    MYSQL *conn = mysql_init(nullptr);
    if (!conn) {
        std::cerr << "mysql_init() failed\n";
        return 1;
    }
    
    if (!mysql_real_connect(conn, "localhost", "root", "", "recovery_test", 3306, nullptr, 0)) {
        std::cerr << "Connection failed: " << mysql_error(conn) << "\n";
        mysql_close(conn);
        return 1;
    }
    
    mysql_set_server_option(conn, MYSQL_OPTION_MULTI_STATEMENTS_ON);
    mysql_query(conn, "SET SQL_MODE='NO_ENGINE_SUBSTITUTION'");
    mysql_query(conn, "SET autocommit=0");
    
    time_t start = time(nullptr);
    
    for (int start_id = 1; start_id <= TOTAL_RECORDS; start_id += BATCH_SIZE) {
        int end_id = std::min(start_id + BATCH_SIZE - 1, TOTAL_RECORDS);
        
        std::string sql = "INSERT INTO test_recovery_big (id, user_name, email, age, created_at, status, data) VALUES ";
        bool first = true;
        
        for (int i = start_id; i <= end_id; i++) {
            int year = 2020 + (i % 4);
            int month = 1 + (i % 12);
            int day = 1 + (i % 28);
            
            std::string user_name = "user_" + std::string(8 - std::to_string(i).length(), '0') + std::to_string(i);
            std::string email = gen_email(i);
            int age = 18 + (i % 80);
            std::string created_at = std::to_string(year) + "-" + 
                std::string(2 - std::to_string(month).length(), '0') + std::to_string(month) + "-" +
                std::string(2 - std::to_string(day).length(), '0') + std::to_string(day) + " 12:00:00";
            int status = i % 3;
            std::string data = gen_data(i);
            
            if (!first) sql += ",";
            sql += "(" + std::to_string(i) + ", '" + user_name + "', '" + email + "', " + 
                   std::to_string(age) + ", '" + created_at + "', " + std::to_string(status) + ", '" + data + "')";
            first = false;
        }
        
        sql += ";";
        
        if (mysql_query(conn, sql.c_str())) {
            std::cerr << "Insert failed: " << mysql_error(conn) << "\n";
            break;
        }
        
        mysql_commit(conn);
        
        double elapsed = difftime(time(nullptr), start);
        std::cout << "Inserted " << end_id << "/" << TOTAL_RECORDS << " ("
                  << (end_id * 100.0 / TOTAL_RECORDS) << "%) - " 
                  << elapsed << "s\n";
    }
    
    // 最终统计
    MYSQL_RES *res = mysql_query(conn, "SELECT COUNT(*) FROM test_recovery_big");
    if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        std::cout << "\nTotal records: " << row[0] << "\n";
        mysql_free_result(res);
    }
    
    mysql_close(conn);
    return 0;
}
