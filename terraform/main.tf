# =============================================================================
# MySQL Recovery Pro - Chaos Testing Infrastructure
# Architect: jliang.fang (Global MySQL Kernel QA & Chaos Engineering)
# Purpose: Provision AWS/Docker infrastructure for multi-version recovery testing
# =============================================================================

terraform {
  required_version = ">= 1.5.0"
  
  required_providers {
    docker = {
      source  = "kreuzwerker/docker"
      version = "~> 3.0"
    }
    null = {
      source  = "hashicorp/null"
      version = "~> 3.0"
    }
  }
}

# ==================== Provider Configuration ====================

provider "docker" {
  host = var.docker_host
}

# ==================== Docker Network ====================

resource "docker_network" "chaos_network" {
  name = "mysql_chaos_network"
  
  driver = "bridge"
  
  labels {
    label = "purpose"
    value = "mysql_recovery_chaos_testing"
  }
}

# ==================== MySQL Container Definitions ====================

# MySQL 5.0 (Antelope format, Compact/Redundant row format)
resource "docker_container" "mysql_50" {
  name = "mysql_chaos_50"
  image = "mysql:5.0.96"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  ports {
    internal = 3306
    external = var.mysql_50_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  volumes {
    host_path = "${path.module}/../docker/init-db"
    container_path = "/docker-entrypoint-initdb.d"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "2g"
  memswap_limit = "4g"
  
  healthcheck {
    test = ["CMD", "mysqladmin", "ping", "-h", "localhost"]
    interval = "10s"
    timeout = "5s"
    retries = 30
  }
  
  depends_on = [docker_network.chaos_network]
}

# MySQL 5.6 (Barracuda + Large Prefix support)
resource "docker_container" "mysql_56" {
  name = "mysql_chaos_56"
  image = "mysql:5.6.51"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  command = [
    "--innodb_file_format=Barracuda",
    "--innodb_file_per_table=1",
    "--innodb_large_prefix=1",
    "--innodb_print_all_deadlocks=1"
  ]
  
  ports {
    internal = 3306
    external = var.mysql_56_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "2g"
  memswap_limit = "4g"
  
  depends_on = [docker_network.chaos_network]
}

# MySQL 5.7 (Dynamic row format, Barracuda)
resource "docker_container" "mysql_57" {
  name = "mysql_chaos_57"
  image = "mysql:5.7.44"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  command = [
    "--innodb_file_format=Barracuda",
    "--innodb_file_per_table=1",
    "--innodb_large_prefix=1",
    "--sql_mode=STRICT_ALL_TABLES"
  ]
  
  ports {
    internal = 3306
    external = var.mysql_57_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "3g"
  memswap_limit = "6g"
  
  depends_on = [docker_network.chaos_network]
}

# MySQL 8.0 (SDI Page 3, MySQL 8.0 Dictionary)
resource "docker_container" "mysql_80" {
  name = "mysql_chaos_80"
  image = "mysql:8.0.36"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  command = [
    "--innodb_file_per_table=1",
    "--innodb_print_all_deadlocks=1",
    "--binlog_format=ROW",
    "--default_authentication_plugin=mysql_native_password"
  ]
  
  ports {
    internal = 3306
    external = var.mysql_80_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "4g"
  memswap_limit = "8g"
  
  depends_on = [docker_network.chaos_network]
}

# MySQL 8.4 (Thomas/Puhui format, 64KB pages)
resource "docker_container" "mysql_84" {
  name = "mysql_chaos_84"
  image = "mysql:8.4.0"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  command = [
    "--innodb_file_per_table=1",
    "--innodb_page_size=16K",
    "--innodb_checksum_algorithm=crc32",
    "--binlog_format=ROW"
  ]
  
  ports {
    internal = 3306
    external = var.mysql_84_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "4g"
  memswap_limit = "8g"
  
  depends_on = [docker_network.chaos_network]
}

# MySQL 9.0 (Future-Proof)
resource "docker_container" "mysql_90" {
  name = "mysql_chaos_90"
  image = "mysql:9.0.0"
  
  env = [
    "MYSQL_ROOT_PASSWORD=${var.mysql_root_password}",
    "MYSQL_DATABASE=recovery_test",
  ]
  
  command = [
    "--innodb_file_per_table=1",
    "--innodb_page_size=16K",
    "--innodb_checksum_algorithm=crc32"
  ]
  
  ports {
    internal = 3306
    external = var.mysql_90_port
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = "${path.module}/../scripts/chaos_injection"
    container_path = "/chaos/injection"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "4g"
  memswap_limit = "8g"
  
  depends_on = [docker_network.chaos_network]
}

# ==================== Chaos Orchestrator ====================

resource "docker_container" "chaos_orchestrator" {
  name = "mysql_chaos_orchestrator"
  image = "python:3.11-slim"
  
  command = ["python3", "/app/scripts/chaos_orchestrator.py"]
  
  volumes {
    host_path = "${path.module}/.."
    container_path = "/app/mysql_recovery_pro"
  }
  
  volumes {
    host_path = var.chaos_volumes_dir
    container_path = "/chaos/volumes"
  }
  
  volumes {
    host_path = var.chaos_results_dir
    container_path = "/chaos/results"
  }
  
  networks_advanced {
    name = docker_network.chaos_network.name
  }
  
  mem_limit = "8g"
  memswap_limit = "16g"
  
  depends_on = [
    docker_container.mysql_50,
    docker_container.mysql_56,
    docker_container.mysql_57,
    docker_container.mysql_80,
    docker_container.mysql_84,
    docker_container.mysql_90
  ]
}

# ==================== Variables ====================

variable "docker_host" {
  description = "Docker daemon host"
  type = string
  default = "unix:///var/run/docker.sock"
}

variable "mysql_root_password" {
  description = "MySQL root password for all containers"
  type = string
  default = "Chaos2026!"
}

variable "chaos_volumes_dir" {
  description = "Host directory for chaos test volumes"
  type = string
  default = "./chaos-test-volumes"
}

variable "chaos_results_dir" {
  description = "Host directory for chaos test results"
  type = string
  default = "./chaos-test-results"
}

variable "mysql_50_port" {
  description = "Host port for MySQL 5.0 container"
  type = number
  default = 33060
}

variable "mysql_56_port" {
  description = "Host port for MySQL 5.6 container"
  type = number
  default = 33061
}

variable "mysql_57_port" {
  description = "Host port for MySQL 5.7 container"
  type = number
  default = 33062
}

variable "mysql_80_port" {
  description = "Host port for MySQL 8.0 container"
  type = number
  default = 33063
}

variable "mysql_84_port" {
  description = "Host port for MySQL 8.4 container"
  type = number
  default = 33064
}

variable "mysql_90_port" {
  description = "Host port for MySQL 9.0 container"
  type = number
  default = 33065
}

# ==================== Outputs ====================

output "mysql_endpoints" {
  value = {
    mysql_50 = "localhost:${var.mysql_50_port}"
    mysql_56 = "localhost:${var.mysql_56_port}"
    mysql_57 = "localhost:${var.mysql_57_port}"
    mysql_80 = "localhost:${var.mysql_80_port}"
    mysql_84 = "localhost:${var.mysql_84_port}"
    mysql_90 = "localhost:${var.mysql_90_port}"
  }
  description = "MySQL container endpoints"
}

output "chaos_results_path" {
  value = var.chaos_results_dir
  description = "Path to chaos test results"
}
