# Hoshizora Configuration Example
# System with nginx, postgres, and Starfield monitoring

system "production-server" {
    # System intents - what we want the system to achieve
    intents {
        web_server_active: true;
        database_ready: file-exists("/var/lib/postgresql/data/PG_VERSION");
        logging_enabled: true;
    }
    
    # Nginx web server service
    service nginx {
        requires: [network_ready];
        exec: "/usr/sbin/nginx" with args ["-g", "daemon off;"];
        respawn: backoff(max = 5, base = 1s);
        capabilities: [net_bind_service()];
        transactional: true;
        memory-limit: 256MiB;
        cpu-weight: 50;
        start-condition: file-exists("/etc/nginx/nginx.conf") and link-up("eth0");
        healthy: tcp-probe("127.0.0.1:80", 5s);
        environment: {
            "NGINX_HOST": "localhost",
            "NGINX_PORT": "80"
        };
    }
    
    # PostgreSQL database service
    service postgres {
        requires: [];
        exec: "/usr/lib/postgresql/bin/postgres" with args ["-D", "/var/lib/postgresql/data"];
        respawn: backoff(max = 3, base = 5s);
        capabilities: [chown(), dac_override()];
        transactional: true;
        snapshot: "/run/hoshizora/snapshots/postgres" every 1h;
        memory-limit: 1GiB;
        cpu-weight: 80;
        start-condition: file-exists("/var/lib/postgresql/data/PG_VERSION");
        healthy: tcp-probe("127.0.0.1:5432", 10s);
        environment: {
            "PGDATA": "/var/lib/postgresql/data",
            "PGPORT": "5432"
        };
    }
    
    # Watch configuration files for changes
    watch "/etc/nginx/nginx.conf" {
        on-change: reload(nginx);
    }
    
    watch "/etc/postgresql/" recursive {
        on-change: restart(postgres);
    }
    
    # Starfield filesystem monitoring
    starfield {
        enabled: true;
        log-size: 64MiB;
        exclude-paths: ["/dev", "/proc", "/sys", "/run/hoshizora"];
    }
}
