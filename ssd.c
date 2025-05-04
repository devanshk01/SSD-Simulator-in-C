/*
-----*----- SSD Simulator in C -----*-----

    MC214 - Operating Systems
    Date : 22-Nov-2024

    Team:
    1. Devansh Kukadia - 202303030
    2. Virat Shrimali - 202303061
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>


/*
    Constants for SSD types and Page states
*/

#define TYPE_DIRECT 1
#define TYPE_LOGGING 2
#define TYPE_IDEAL 3
#define STATE_INVALID 1
#define STATE_ERASED 2
#define STATE_VALID 3


// Maximum size
#define MAX_PAGES 1000
#define MAX_BLOCKS 100


/*
    SSD Structural Definition
*/

typedef struct {
    int ssd_type;
    int num_logical_pages;
    int num_blocks;
    int pages_per_block;
    float block_erase_time;
    float page_program_time;
    float page_read_time;
    int gc_high_water_mark;
    int gc_low_water_mark;
    int gc_trace;
    int show_state;

    int num_pages;
    int state[MAX_PAGES];
    char data[MAX_PAGES];
    int current_page;
    int current_block;
    int gc_count;
    int gc_current_block;
    int gc_used_blocks[MAX_BLOCKS];
    int live_count[MAX_BLOCKS];
    int forward_map[MAX_PAGES];
    int reverse_map[MAX_PAGES];

    int physical_erase_count[MAX_BLOCKS];
    int physical_read_count[MAX_BLOCKS];
    int physical_write_count[MAX_BLOCKS];
    int physical_erase_sum;
    int physical_write_sum;
    int physical_read_sum;
    int logical_trim_sum;
    int logical_write_sum;
    int logical_read_sum;
    int logical_trim_fail_sum;
    int logical_write_fail_sum;
    int logical_read_fail_sum;
} SSD;


/* 
    Implicit Function declaration 
*/

int blocks_in_use(SSD *s);
void physical_erase(SSD *s, int block_address);
void physical_program(SSD *s, int page_address, char data);
char physical_read(SSD *s, int page_address);
char *read_direct(SSD *s, int address);
char *write_direct(SSD *s, int page_address, char data);
char *write_ideal(SSD *s, int page_address, char data);
int is_block_free(SSD *s, int block);
int get_cursor(SSD *s);
void update_cursor(SSD *s);
char *write_logging(SSD *s, int page_address, char data, int is_gc_write);
void garbage_collect(SSD *s);
void upkeep(SSD *s);
char *trim(SSD *s, int address);
char *read_ssd(SSD *s, int address);
char *write_ssd(SSD *s, int address, char data);
char printable_state(int s);
void stats(SSD *s);
void dump(SSD *s);
void initialize_ssd (SSD *s, int ssd_type, int num_logical_pages, int num_blocks,
                    int pages_per_block, float block_erase_time, float page_program_time,
                    float page_read_time, int high_water_mark, int low_water_mark,
                    int trace_gc, int show_state);


/*
    Implementation of Functions
*/

void initialize_ssd(SSD *s, int ssd_type, int num_logical_pages, int num_blocks, int pages_per_block,
                    float block_erase_time, float page_program_time, float page_read_time,
                    int high_water_mark, int low_water_mark, int trace_gc, int show_state) {
    s->ssd_type = ssd_type;
    s->num_logical_pages = num_logical_pages;
    s->num_blocks = num_blocks;
    s->pages_per_block = pages_per_block;
    s->block_erase_time = block_erase_time;
    s->page_program_time = page_program_time;
    s->page_read_time = page_read_time;
    s->gc_high_water_mark = high_water_mark;
    s->gc_low_water_mark = low_water_mark;
    s->gc_trace = trace_gc;
    s->show_state = show_state;

    s->num_pages = s->num_blocks * s->pages_per_block;
    for (int i = 0; i < s->num_pages; i++) {
        s->state[i] = STATE_INVALID;
        s->data[i] = ' ';
    }

    s->current_page = -1;
    s->current_block = 0;

    // gc counts
    s->gc_count = 0;
    s->gc_current_block = 0;

    for (int i = 0; i < s->num_blocks; i++) {
        
        // can use this as a log block
        s->gc_used_blocks[i] = 0;

        // counts so as to help the GC
        s->live_count[i] = 0;

        // stats
        s->physical_erase_count[i] = 0;
        s->physical_read_count[i] = 0;
        s->physical_write_count[i] = 0;
    }

    s->physical_erase_sum = 0;
    s->physical_write_sum = 0;
    s->physical_read_sum = 0;

    s->logical_trim_sum = 0;
    s->logical_write_sum = 0;
    s->logical_read_sum = 0;

    s->logical_trim_fail_sum = 0;
    s->logical_write_fail_sum = 0;
    s->logical_read_fail_sum = 0;

    for (int i = 0; i < s->num_logical_pages; i++) {
        s->forward_map[i] = -1;
    }

    for (int i = 0; i < s->num_pages; i++) {
        s->reverse_map[i] = -1;
    }
}

int blocks_in_use(SSD *s) {
    int used = 0;
    for (int i = 0; i < s->num_blocks; i++) {
        used += s->gc_used_blocks[i];
    }
    return used;
}

void physical_erase(SSD *s, int block_address) {
    int page_begin = block_address * s->pages_per_block;
    int page_end = page_begin + s->pages_per_block - 1;

    for (int page = page_begin; page <= page_end; page++) {
        s->data[page] = ' ';
        s->state[page] = STATE_ERASED;
    }

    // definitely NOT in use
    s->gc_used_blocks[block_address] = 0;

    // stats
    s->physical_erase_count[block_address]++;
    s->physical_erase_sum++;
}

void physical_program(SSD *s, int page_address, char data) {
    s->data[page_address] = data;
    s->state[page_address] = STATE_VALID;

    // stats
    s->physical_write_count[page_address / s->pages_per_block]++;
    s->physical_write_sum++;
}

char physical_read(SSD *s, int page_address) {

    // stats
    s->physical_read_count[page_address / s->pages_per_block]++;
    s->physical_read_sum++;
    return s->data[page_address];
}

char *read_direct(SSD *s, int address) {
    static char result[2];
    result[0] = physical_read(s, address);
    result[1] = '\0';
    return result;
}

char *write_direct(SSD *s, int page_address, char data) {
    int block_address = page_address / s->pages_per_block;
    int page_begin = block_address * s->pages_per_block;
    int page_end = page_begin + s->pages_per_block - 1;

    int old_list_pages[MAX_PAGES];
    char old_list_data[MAX_PAGES];
    int old_list_count = 0;

    for (int old_page = page_begin; old_page <= page_end; old_page++) {
        if (s->state[old_page] == STATE_VALID) {
            char old_data = physical_read(s, old_page);
            old_list_pages[old_list_count] = old_page;
            old_list_data[old_list_count] = old_data;
            old_list_count++;
        }
    }

    physical_erase(s, block_address);
    for (int i = 0; i < old_list_count; i++) {
        int old_page = old_list_pages[i];
        char old_data = old_list_data[i];
        if (old_page == page_address) {
            continue;
        }
        physical_program(s, old_page, old_data);
    }

    physical_program(s, page_address, data);
    s->forward_map[page_address] = page_address;
    s->reverse_map[page_address] = page_address;
    return "success";
}

char *write_ideal(SSD *s, int page_address, char data) {
    physical_program(s, page_address, data);
    s->forward_map[page_address] = page_address;
    s->reverse_map[page_address] = page_address;
    return "success";
}

int is_block_free(SSD *s, int block) {
    int first_page = block * s->pages_per_block;
    if (s->state[first_page] == STATE_INVALID || s->state[first_page] == STATE_ERASED) {
        if (s->state[first_page] == STATE_INVALID) {
            physical_erase(s, block);
        }
        s->current_block = block;
        s->current_page = first_page;
        s->gc_used_blocks[block] = 1;
        return 1;
    }
    return 0;
}

int get_cursor(SSD *s) {
    if (s->current_page == -1) {
        for (int block = s->current_block; block < s->num_blocks; block++) {
            if (is_block_free(s, block)) {
                return 0;
            }
        }
        for (int block = 0; block < s->current_block; block++) {
            if (is_block_free(s, block)) {
                return 0;
            }
        }
        return -1;
    }
    return 0;
}

void update_cursor(SSD *s) {
    s->current_page++;
    if (s->current_page % s->pages_per_block == 0) {
        s->current_page = -1;
    }
}

char *write_logging(SSD *s, int page_address, char data, int is_gc_write) {
    if (get_cursor(s) == -1) {
        s->logical_write_fail_sum++;
        return "failure: device full";
    }

    // normal mode writing
    physical_program(s, s->current_page, data);
    s->forward_map[page_address] = s->current_page;
    s->reverse_map[s->current_page] = page_address;
    update_cursor(s);
    return "success";
}

void garbage_collect(SSD *s) {

    int blocks_cleaned = 0;

    for (int i = 0; i < s->num_blocks; i++) {
        int block = (s->gc_current_block + i) % s->num_blocks;

        // don't GC the block currently being written to
        if (block == s->current_block) {
            continue;
        }

        // page to start looking for live blocks
        int page_start = block * s->pages_per_block;

        // if this page (and hence block) already erased, then do not bother
        if (s->state[page_start] == STATE_ERASED) {
            continue;
        }

        // collect list of live physical pages in this block
        int live_pages[MAX_PAGES];
        int live_count = 0;
        for (int page = page_start; page < page_start + s->pages_per_block; page++) {
            int logical_page = s->reverse_map[page];
            if (logical_page != -1 && s->forward_map[logical_page] == page) {
                live_pages[live_count++] = page;
            }
        }

        // if only live blocks, then don't clean it
        if (live_count == s->pages_per_block) {
            continue;
        }

        // live pages should be copied to current writing location
        for (int i = 0; i < live_count; i++) {
            int page = live_pages[i];

            // live, so copy it someplace new
            if (s->gc_trace) {
                printf("gc %d:: read(physical_page=%d)\n", s->gc_count, page);
                printf("gc %d:: write()\n", s->gc_count);
            }
            char data = physical_read(s, page);
            write_ssd(s, s->reverse_map[page], data);
        }

        // finally, erase the block and see if we're done
        blocks_cleaned++;
        physical_erase(s, block);

        if (s->gc_trace) {
            printf("gc %d:: erase(block=%d)\n", s->gc_count, block);
            if (s->show_state) {
                printf("\n");
                dump(s);
                printf("\n");
            }
        }

        if (blocks_in_use(s) <= s->gc_low_water_mark) {

            // record where we stopped and return
            s->gc_current_block = block;
            s->gc_count++;
            return;
        }
    }

    // END: block iteration
}

void upkeep(SSD *s) {

    // GARBAGE COLLECTION
    if (blocks_in_use(s) >= s->gc_high_water_mark) {
        garbage_collect(s);
    }
    // WEAR LEVELING: for future
}

char *trim(SSD *s, int address) {
    s->logical_trim_sum++;
    if (address < 0 || address >= s->num_logical_pages) {
        s->logical_trim_fail_sum++;
        return "fail: illegal trim address";
    }
    if (s->forward_map[address] == -1) {
        s->logical_trim_fail_sum++;
        return "fail: uninitialized trim";
    }
    s->forward_map[address] = -1;
    return "success";
}

char *read_ssd(SSD *s, int address) {
    s->logical_read_sum++;
    if (address < 0 || address >= s->num_logical_pages) {
        s->logical_read_fail_sum++;
        return "fail: illegal read address";
    }
    if (s->forward_map[address] == -1) {
        s->logical_read_fail_sum++;
        return "fail: uninitialized read";
    }

    // USED for DIRECT and LOGGING and IDEAL
    return read_direct(s, s->forward_map[address]);
}

char *write_ssd(SSD *s, int address, char data) {
    s->logical_write_sum++;
    if (address < 0 || address >= s->num_logical_pages) {
        s->logical_write_fail_sum++;
        return "fail: illegal write address";
    }
    if (s->ssd_type == TYPE_DIRECT) {
        return write_direct(s, address, data);
    } else if (s->ssd_type == TYPE_IDEAL) {
        return write_ideal(s, address, data);
    } else {
        return write_logging(s, address, data, 0);
    }
}

char printable_state(int s) {
    if (s == STATE_INVALID) {
        return 'i';
    } else if (s == STATE_ERASED) {
        return 'E';
    } else if (s == STATE_VALID) {
        return 'v';
    } else {
        printf("bad state %d\n", s);
        exit(1);
    }
}

void stats(SSD *s) {
    printf("Physical Operations Per Block\n");
    printf("Erases ");
    for (int i = 0; i < s->num_blocks; i++) {
        printf("%3d        ", s->physical_erase_count[i]);
    }
    printf("  Sum: %d\n", s->physical_erase_sum);

    printf("Writes ");
    for (int i = 0; i < s->num_blocks; i++) {
        printf("%3d        ", s->physical_write_count[i]);
    }
    printf("  Sum: %d\n", s->physical_write_sum);

    printf("Reads  ");
    for (int i = 0; i < s->num_blocks; i++) {
        printf("%3d        ", s->physical_read_count[i]);
    }
    printf("  Sum: %d\n", s->physical_read_sum);
    printf("\n");
    printf("Logical Operation Sums\n");
    printf("  Write count %d (%d failed)\n", s->logical_write_sum, s->logical_write_fail_sum);
    printf("  Read count  %d (%d failed)\n", s->logical_read_sum, s->logical_read_fail_sum);
    printf("  Trim count  %d (%d failed)\n", s->logical_trim_sum, s->logical_trim_fail_sum);
    printf("\n");
    printf("Times\n");
    printf("  Erase time %.2f\n", s->physical_erase_sum * s->block_erase_time);
    printf("  Write time %.2f\n", s->physical_write_sum * s->page_program_time);
    printf("  Read time  %.2f\n", s->physical_read_sum * s->page_read_time);
    float total_time = s->physical_erase_sum * s->block_erase_time +
                       s->physical_write_sum * s->page_program_time +
                       s->physical_read_sum * s->page_read_time;
    printf("  Total time %.2f\n", total_time);
}

void dump(SSD *s) {

    // FTL
    printf("FTL   ");
    int count = 0;
    int ftl_columns = (s->pages_per_block * s->num_blocks) / 7;
    for (int i = 0; i < s->num_logical_pages; i++) {
        if (s->forward_map[i] == -1) {
            continue;
        }
        count++;
        printf("%3d:%3d ", i, s->forward_map[i]);
        if (count > 0 && count % ftl_columns == 0) {
            printf("\n      ");
        }
    }
    if (count == 0) {
        printf("(empty)");
    }
    printf("\n");

    // Blocks
    printf("Block ");
    for (int i = 0; i < s->num_blocks; i++) {
        printf("%d", i);
        for (int j = 0; j < s->pages_per_block - 1; j++) {
            printf(" ");
        }
        printf(" ");
    }
    printf("\n");

    // Pages
    int max_len = snprintf(NULL, 0, "%d", s->num_pages - 1);
    for (int n = max_len; n > 0; n--) {
        if (n == max_len) {
            printf("Page  ");
        } else {
            printf("      ");
        }
        for (int i = 0; i < s->num_pages; i++) {
            char buf[10];
            snprintf(buf, sizeof(buf), "%0*d", max_len, i);
            printf("%c", buf[max_len - n]);
            if (i > 0 && (i + 1) % 10 == 0) {
                printf(" ");
            }
        }
        printf("\n");
    }

    // State
    printf("State ");
    for (int i = 0; i < s->num_pages; i++) {
        printf("%c", printable_state(s->state[i]));
        if (i > 0 && (i + 1) % 10 == 0) {
            printf(" ");
        }
    }
    printf("\n");

    // Data
    printf("Data  ");
    for (int i = 0; i < s->num_pages; i++) {
        if (s->state[i] == STATE_VALID) {
            printf("%c", s->data[i]);
        } else {
            printf(" ");
        }
        if (i > 0 && (i + 1) % 10 == 0) {
            printf(" ");
        }
    }
    printf("\n");

    // Live
    printf("Live  ");
    for (int i = 0; i < s->num_pages; i++) {
        if (s->state[i] == STATE_VALID && s->forward_map[s->reverse_map[i]] == i) {
            printf("+");
        } else {
            printf(" ");
        }
        if (i > 0 && (i + 1) % 10 == 0) {
            printf(" ");
        }
    }
    printf("\n");
}


/*
    Driver Code
*/

int main(int argc, char *argv[]) {

    int seed = 0;
    int num_cmds = 10;
    char op_percentages[100] = "40/50/10";
    char skew[100] = "";
    int skew_start = 0;
    int read_fail = 0;
    char cmd_list[1000] = "";
    char ssd_type_str[10] = "direct";
    int num_logical_pages = 50;
    int num_blocks = 7;
    int pages_per_block = 10;
    int high_water_mark = 10;
    int low_water_mark = 8;
    int read_time = 10;
    int program_time = 40;
    int erase_time = 1000;
    int show_gc = 0;
    int show_state = 0;
    int show_cmds = 0;
    int quiz_cmds = 0;
    int show_stats = 0;
    int solve = 0;

    int opt;
    while ((opt = getopt(argc, argv, "s:n:P:K:k:r:L:T:l:B:p:G:g:R:W:E:JFCqSc")) != -1) {
        switch (opt) {
            case 's':
                seed = atoi(optarg);
                break;
            case 'n':
                num_cmds = atoi(optarg);
                break;
            case 'P':
                strncpy(op_percentages, optarg, sizeof(op_percentages));
                break;
            case 'K':
                strncpy(skew, optarg, sizeof(skew));
                break;
            case 'k':
                skew_start = atoi(optarg);
                break;
            case 'r':
                read_fail = atoi(optarg);
                break;
            case 'L':
                strncpy(cmd_list, optarg, sizeof(cmd_list));
                break;
            case 'T':
                strncpy(ssd_type_str, optarg, sizeof(ssd_type_str));
                break;
            case 'l':
                num_logical_pages = atoi(optarg);
                break;
            case 'B':
                num_blocks = atoi(optarg);
                break;
            case 'p':
                pages_per_block = atoi(optarg);
                break;
            case 'G':
                high_water_mark = atoi(optarg);
                break;
            case 'g':
                low_water_mark = atoi(optarg);
                break;
            case 'R':
                read_time = atoi(optarg);
                break;
            case 'W':
                program_time = atoi(optarg);
                break;
            case 'E':
                erase_time = atoi(optarg);
                break;
            case 'J':
                show_gc = 1;
                break;
            case 'F':
                show_state = 1;
                break;
            case 'C':
                show_cmds = 1;
                break;
            case 'q':
                quiz_cmds = 1;
                break;
            case 'S':
                show_stats = 1;
                break;
            case 'c':
                solve = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [options]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }


    // Print the list of CL arguments

    printf("ARG seed %d\n", seed);
    printf("ARG num_cmds %d\n", num_cmds);
    printf("ARG op_percentages %s\n", op_percentages);
    printf("ARG skew %s\n", skew);
    printf("ARG skew_start %d\n", skew_start);
    printf("ARG read_fail %d\n", read_fail);
    printf("ARG cmd_list %s\n", cmd_list);
    printf("ARG ssd_type %s\n", ssd_type_str);
    printf("ARG num_logical_pages %d\n", num_logical_pages);
    printf("ARG num_blocks %d\n", num_blocks);
    printf("ARG pages_per_block %d\n", pages_per_block);
    printf("ARG high_water_mark %d\n", high_water_mark);
    printf("ARG low_water_mark %d\n", low_water_mark);
    printf("ARG erase_time %d\n", erase_time);
    printf("ARG program_time %d\n", program_time);
    printf("ARG read_time %d\n", read_time);
    printf("ARG show_gc %d\n", show_gc);
    printf("ARG show_state %d\n", show_state);
    printf("ARG show_cmds %d\n", show_cmds);
    printf("ARG quiz_cmds %d\n", quiz_cmds);
    printf("ARG show_stats %d\n", show_stats);
    printf("ARG compute %d\n", solve);
    printf("\n");


    // Initialize SSD object 

    SSD s;
    int ssd_type;
    if (strcmp(ssd_type_str, "direct") == 0) {
        ssd_type = TYPE_DIRECT;
    } else if (strcmp(ssd_type_str, "log") == 0) {
        ssd_type = TYPE_LOGGING;
    } else if (strcmp(ssd_type_str, "ideal") == 0) {
        ssd_type = TYPE_IDEAL;
    } else {
        printf("bad SSD type (%s)\n", ssd_type_str);
        exit(1);
    }

    initialize_ssd(&s, ssd_type, num_logical_pages, num_blocks, pages_per_block,
                   (float)erase_time, (float)program_time, (float)read_time,
                   high_water_mark, low_water_mark, show_gc, show_state);


    // generate cmds (if not passed in by cmd_list)

    srand(seed);
    
    char cmds[1000][100];
    int cmd_count = 0;

    if (strlen(cmd_list) == 0) {
        int max_page_addr = num_logical_pages;
        int percent_reads, percent_writes, percent_trims;
        sscanf(op_percentages, "%d/%d/%d", &percent_reads, &percent_writes, &percent_trims);

        if (percent_writes <= 0) {
            printf("must have some writes, otherwise nothing in the SSD!\n");
            exit(1);
        }

        char printable[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int valid_addresses[MAX_PAGES];
        int valid_address_count = 0;
        int total_percent = percent_reads + percent_writes + percent_trims;

        for (int i = 0; i < num_cmds; i++) {
            int which_cmd = rand() % total_percent;
            if (which_cmd < percent_reads) {

                // read
                int address;
                if (rand() % 100 < read_fail) {
                    address = rand() % max_page_addr;
                } else {
                    if (valid_address_count < 2) {
                        i--;
                        continue;
                    }
                    address = valid_addresses[rand() % valid_address_count];
                }
                sprintf(cmds[cmd_count++], "r%d", address);
            } else if (which_cmd < percent_reads + percent_writes) {

                // write
                int address;
                if (skew_start == 0 && strlen(skew) > 0 && ((float)rand() / RAND_MAX) < (atoi(strtok(skew, "/")) / 100.0)) {
                    address = rand() % (int)(atoi(strtok(NULL, "/")) / 100.0 * (max_page_addr - 1));
                } else {
                    address = rand() % max_page_addr;
                }
                int found = 0;
                for (int j = 0; j < valid_address_count; j++) {
                    if (valid_addresses[j] == address) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    valid_addresses[valid_address_count++] = address;
                }
                char data = printable[rand() % strlen(printable)];
                sprintf(cmds[cmd_count++], "w%d:%c", address, data);
                if (skew_start > 0) {
                    skew_start--;
                }
            } else {

                // trim
                if (valid_address_count < 1) {
                    i--;
                    continue;
                }
                int idx = rand() % valid_address_count;
                int address = valid_addresses[idx];
                sprintf(cmds[cmd_count++], "t%d", address);

                for (int j = idx; j < valid_address_count - 1; j++) {
                    valid_addresses[j] = valid_addresses[j + 1];
                }
                valid_address_count--;
            }
        }
    } else {
        char *token = strtok(cmd_list, ",");
        while (token != NULL) {
            strcpy(cmds[cmd_count++], token);
            token = strtok(NULL, ",");
        }
    }

    dump(&s);
    printf("\n");

    int op = 0;
    for (int i = 0; i < cmd_count; i++) {
        char *cmd = cmds[i];
        if (strlen(cmd) == 0) {
            break;
        }
        if (cmd[0] == 'r') {

            // read
            int address = atoi(cmd + 1);
            char *data = read_ssd(&s, address);
            if (show_cmds || (quiz_cmds && solve)) {
                printf("cmd %3d:: read(%d) -> %s\n", op, address, data);
            } else if (quiz_cmds) {
                printf("cmd %3d:: read(%d) -> ??\n", op, address);
            }
            op++;
        } else if (cmd[0] == 'w') {

            // write
            char *colon = strchr(cmd, ':');
            int address = atoi(cmd + 1);
            char data = colon[1];
            char *rc = write_ssd(&s, address, data);
            if (show_cmds || (quiz_cmds && solve)) {
                printf("cmd %3d:: write(%d, %c) -> %s\n", op, address, data, rc);
            } else if (quiz_cmds) {
                printf("cmd %3d:: command(??) -> ??\n", op);
            }
            op++;
        } else if (cmd[0] == 't') {

            // trim
            int address = atoi(cmd + 1);
            char *rc = trim(&s, address);
            if (show_cmds || (quiz_cmds && solve)) {
                printf("cmd %3d:: trim(%d) -> %s\n", op, address, rc);
            } else if (quiz_cmds) {
                printf("cmd %3d:: command(??) -> ??\n", op);
            }
            op++;
        }

        if (show_state) {
            printf("\n");
            dump(&s);
            printf("\n");
        }

        upkeep(&s);
    }

    if (!show_state) {
        printf("\n");
        dump(&s);
    }
    printf("\n");
    if (show_stats) {
        stats(&s);
        printf("\n");
    }

    return 0;
}
