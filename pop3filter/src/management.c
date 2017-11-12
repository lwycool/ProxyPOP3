#include <sys/socket.h>
#include <unistd.h>
#include <memory.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdlib.h>

#include "selector.h"
#include "parameters.h"
#include "utils.h"
#include "management.h"
#include "media_types.h"
#include "metrics.h"

#define ATTACHMENT(key) ( (struct management *)(key)->data)
#define N(x) (sizeof(x)/sizeof((x)[0]))

options parameters;

void management_read(struct selector_key *key);

void management_write(struct selector_key *key);

void management_close(struct selector_key *key);

static const struct fd_handler management_handler = {
        .handle_read   = management_read,
        .handle_write  = management_write,
        .handle_close  = management_close,
        .handle_block  = NULL,
};

// management struct functions.
struct management *
management_new(const int client_fd){
    struct management *ret;
    ret = malloc(sizeof(*ret));
    if (ret == NULL){
        return ret;
    }

    ret->client_fd     = client_fd;
    buffer_init(&ret->buffer_write, N(ret->raw_buffer_write),ret->raw_buffer_write);
    buffer_init(&ret->buffer_read , N(ret->raw_buffer_read) ,ret->raw_buffer_read);
    ret->status = ST_HELO;
    ret->error  = PARSE_OK;
    return ret;
}

void
management_destroy(struct management * corpse){
    free(corpse);
}

// handler functions

void management_accept_connection(struct selector_key *key) {
    struct sockaddr_storage       client_addr;
    socklen_t                     client_addr_len = sizeof(client_addr);
    struct management                  *state           = NULL;

    const int client = accept(key->fd, (struct sockaddr*) &client_addr,
                              &client_addr_len);
    if(client == -1) {
        goto fail;
    }
    if(selector_fd_set_nio(client) == -1) {
        goto fail;
    }
    print_connection_status("Connection established", client_addr);
    printf("File descriptor: %d\n", client);
    state = management_new(client);
    if(state == NULL) {
        // sin un estado, nos es imposible manejaro.
        // tal vez deberiamos apagar accept() hasta que detectemos
        // que se liberó alguna conexión.
        goto fail;
    }

    memcpy(&state->client_addr, &client_addr, client_addr_len);
    state->client_addr_len = client_addr_len;

    if(SELECTOR_SUCCESS != selector_register(key->s, client, &management_handler,
                                             OP_WRITE, state)) {
        goto fail;
    }

    return ;

    fail:
    if(client != -1) {
        close(client);
    }
    management_destroy(state);
}

void
management_read(struct selector_key *key){
    struct management *data = ATTACHMENT(key);
    if (split_commands(data) < 0 || selector_set_interest(key->s, key->fd, OP_WRITE) != SELECTOR_SUCCESS){
        selector_unregister_fd(key->s, data->client_fd);
    };
}

void
management_write(struct selector_key *key){
    struct management * data = ATTACHMENT(key);
    if (parse_commands(data) < 0 || selector_set_interest(key->s, key->fd, OP_READ) != SELECTOR_SUCCESS){
        selector_unregister_fd(key->s, data->client_fd);
    }
}

void
management_close(struct selector_key *key){
    struct management * data = ATTACHMENT(key);
    print_connection_status("Connection disconnected", data->client_addr);
    management_destroy(data);
}

/* PARSER */

int parse_hello(struct management *data);
int parse_user(struct management *data);
int parse_pass(struct management *data);
int parse_config(struct management *data);
int goodbye(struct management * data);

struct parse_action{
    parse_status status;
    int (* action)(struct management * data);
    int args;
};

static struct parse_action hello_action = {
        .status      = ST_HELO,
        .action      = parse_hello,
        .args        = 0,
};

static struct parse_action user_action = {
        .status      = ST_USER,
        .action      = parse_user,
        .args        = 2,
};

static struct parse_action pass_action = {
        .status      = ST_PASS,
        .action      = parse_pass,
        .args        = 2,
};

static struct parse_action config_action = {
        .status      = ST_CONFIG,
        .action      = parse_config,
        .args        = 0,
};

static struct parse_action * action_list[] = {
        &hello_action,
        &user_action,
        &pass_action,
        &config_action,
};

int split_commands(struct management *data){
    data->argc = 0;
    data->cmd = sctp_parse_cmd(&data->buffer_read, data, &data->argc, (int *) &data->error);
    if (data->error == ERROR_DISCONNECT){
        return -1;
    }
}

int parse_commands(struct management *data){
    struct parse_action * act = action_list[data->status];
    int st_err = data->error;
    int status = 0;

    if (act->args != 0 && act->args != data->argc)
        st_err = ERROR_WRONGARGS;
    if (data->argc == 1 && strcasecmp("QUIT", data->cmd[0]) == 0){
        return goodbye(data);
    }
    switch (st_err){
        case PARSE_OK:
            status = act->action(data);
            free_cmd(data->cmd, act->args);
            break;
        case ERROR_WRONGARGS:
            send_error(data, "wrong command or wrong number of arguments.");
            break;
        case ERROR_MALLOC:
            send_error(data, "server error. Try again later.");
        default:
            break;
    }
    return status;
}

int parse_hello(struct management *data){
    char * message = "POP3 Proxy Management Server.\n";
    send(data->client_fd, message, strlen(message), 0);
    data->status = ST_USER;
    return 0;
}

int parse_user(struct management *data){
    char ** cmd = data->cmd;
    if (strcasecmp("USER", cmd[0]) == 0){
        char * user = malloc(sizeof(char) * strlen(cmd[1]));
        if (user == NULL)
            return -1;
        strcpy(user, cmd[1]);
        data->user = user;
        data->status = ST_PASS;
        send_ok(data, "Welcome");
    }else{
        send_error(data, "command not recognized.");
    }
    return 0;
}

int parse_pass(struct management *data){
    char ** cmd = data->cmd;
    if (strcasecmp("PASS", cmd[0]) == 0){
        if (strcmp(parameters->pass, cmd[1]) == 0 && strcmp(parameters->user, data->user) == 0){
            free(data->user);
            send_ok(data, "Logged in.");
            data->status = ST_CONFIG;
        }else{
            send_error(data, "Authentication failed. Try again.");
            data->status = ST_USER;
        }
    }else{
        send_error(data, "command not recognized.");
    }
    return 0;
}

int parse_config(struct management *data){
    char ** cmd = data->cmd;
    if(strcasecmp("CMD", cmd[0]) == 0){
        if (data->argc == 1) {
            parameters->et_activated = !parameters->et_activated;
            if (parameters->et_activated) {
                send_ok(data, "External transformations activated.");
            } else {
                send_ok(data, "External transformations deactivated.");
            }
            return 0;
        }
        else if (data->argc != 2)
            goto fail;
        char * str = malloc(sizeof(char) * strlen(cmd[1]));
        strcpy(str, cmd[1]);
        if (str == NULL)
            goto fail;
        if (parameters->filter_command != NULL)
            free(parameters->filter_command);
        parameters->filter_command = str;
        goto success;
    }else if(strcasecmp("MSG", cmd[0]) == 0){
        if (data->argc != 2)
            goto fail;
        char * str = malloc(sizeof(char) * strlen(cmd[1]));
        if (str == NULL)
            goto fail_mem;
        strcpy(str, cmd[1]);
        parameters->replacement_msg = cmd[1];
        goto success;
    }else if(strcasecmp("LIST", cmd[0]) == 0){
        if (data->argc != 1)
            goto fail;
        char * msg = get_types_list(parameters->filtered_media_types, '\n');
        send_ok(data, msg);
        free(msg);
        return 0;
    }else if(strcasecmp("BAN", cmd[0]) == 0){
        if (data->argc != 2)
            goto fail;
        char * str = malloc(sizeof(char) * strlen(cmd[1]));
        if (str == NULL)
            goto fail_mem;
        strcpy(str, cmd[1]);
        char * type, * subtype;
        if (is_mime(str, &type, &subtype) < 0)
            goto fail_type;
        char * s = malloc(sizeof(char) * strlen(subtype));
        if (s == NULL)
            goto fail_mem;
        strcpy(s, subtype);
        if (add_media_type(parameters->filtered_media_types, type, s) < 0){
            send_error(data, "could not ban type");
        }else{
            send_ok(data, "type banned");
        }
        return 0;
    }else if(strcasecmp("UNBAN", cmd[0]) == 0){
        if (data->argc != 2)
            goto fail;
        char * type, * subtype;
        if (is_mime(cmd[1], &type, &subtype) < 0)
            goto fail_type;
        if (delete_media_type(parameters->filtered_media_types, type, subtype) < 0){
            send_error(data, "could not unban type");
        }else{
            send_ok(data, "type unbanned");
        }
        return 0;
    }else if(strcasecmp("STATS", cmd[0]) == 0) {
        if (data->argc != 1)
            goto fail;
        char msg[200];
        sprintf(msg, "\nMetrics\n"
                        "Concurrent connections: %u\n"
                        "Historical Access: %u\n"
                        "Transfered Bytes: %lld\n"
                        "Retrieved Messages: %u\n", metricas->concurrent_connections,
                metricas->historical_access, metricas->transferred_bytes,
                metricas->retrieved_messages);
        send_ok(data, msg);
        return 0;
    }
    fail:
    send_error(data, "command not recognized.");
    return 0;
    fail_mem:
    send_error(data, "server error.");
    return 0;
    fail_type:
    send_error(data, "wrong media type.");
    return 0;
    success:
    send_ok(data, "Done.");
    return 0;
}

int goodbye(struct management * data){
    send_ok(data, "Goodbye.");
    close(data->client_fd);
    return -1;
}