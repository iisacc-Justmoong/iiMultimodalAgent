#pragma once
#include <iiMultimodalAgent/Export.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iiquestion_presentation iiquestion_presentation;
typedef struct iiquestion_model_lease iiquestion_model_lease;
typedef struct iiquestion_option_data { const char *label, *description, *preview; } iiquestion_option_data;
typedef struct iiquestion_question_data {
    const char *text, *header;
    int multiple;
    const iiquestion_option_data *options;
    size_t option_count;
} iiquestion_question_data;
typedef struct iiquestion_request_data {
    const char *id;
    const iiquestion_question_data *questions;
    size_t question_count;
} iiquestion_request_data;
typedef struct iiquestion_strings {
    const char *title, *description, *waiting, *other, *notes, *other_accessibility, *notes_accessibility;
    const char *decline, *skip, *send;
} iiquestion_strings;
typedef struct iiquestion_response_item {
    const char *question, *answer, *preview, *notes; /* absent fields are NULL */
} iiquestion_response_item;
typedef void (*iiquestion_observer)(void *context, int property, size_t row, size_t option, uint64_t generation);
/* Return 1 when accepted. All data is borrowed for the callback duration. */
typedef int (*iiquestion_action)(void *context, int action, const char *request_id,
                                 const iiquestion_response_item *items, size_t count);
typedef void (*iiquestion_read_callback)(void *context, const iiquestion_request_data *requests,
                                         size_t count, const char *error);

enum iiquestion_property {
    IIQUESTION_ROWS, IIQUESTION_OPEN, IIQUESTION_HAS_WAITING, IIQUESTION_HAS_ERROR, IIQUESTION_MOTION_ENABLED,
    IIQUESTION_TITLE, IIQUESTION_DESCRIPTION, IIQUESTION_WAITING, IIQUESTION_ERROR,
    IIQUESTION_OTHER_PLACEHOLDER, IIQUESTION_NOTES_PLACEHOLDER, IIQUESTION_DECLINE_TEXT,
    IIQUESTION_SKIP_TEXT, IIQUESTION_SEND_TEXT,
    IIQUESTION_ROW_ID, IIQUESTION_ROW_TEXT, IIQUESTION_ROW_HEADER, IIQUESTION_ROW_MULTIPLE,
    IIQUESTION_ROW_OTHER, IIQUESTION_ROW_NOTES, IIQUESTION_ROW_PREVIEW,
    IIQUESTION_ROW_OTHER_ACCESSIBILITY, IIQUESTION_ROW_NOTES_ACCESSIBILITY,
    IIQUESTION_OPTION_ID, IIQUESTION_OPTION_LABEL, IIQUESTION_OPTION_DESCRIPTION,
    IIQUESTION_OPTION_PREVIEW, IIQUESTION_OPTION_CHECKED
};
enum iiquestion_action_kind { IIQUESTION_ACTION_SUBMIT, IIQUESTION_ACTION_DECLINE };
enum iiquestion_command { IIQUESTION_SUBMIT, IIQUESTION_SKIP, IIQUESTION_DECLINE };

/* Calls, callbacks and leases are owner-thread only. Row operations require the
 * generation returned by iiquestion_rows; a replaced row is rejected as stale.
 * Release a lease only after all native views, callbacks and queries retire. */
IIMULTIMODALAGENT_EXPORT const char *iiquestion_last_error(void);
IIMULTIMODALAGENT_EXPORT iiquestion_presentation *iiquestion_create(void);
IIMULTIMODALAGENT_EXPORT int iiquestion_destroy(iiquestion_presentation *);
IIMULTIMODALAGENT_EXPORT int iiquestion_observe(iiquestion_presentation *, iiquestion_observer, void *);
IIMULTIMODALAGENT_EXPORT int iiquestion_set_actions(iiquestion_presentation *, iiquestion_action, void *);
IIMULTIMODALAGENT_EXPORT int iiquestion_set_strings(iiquestion_presentation *, const iiquestion_strings *);
IIMULTIMODALAGENT_EXPORT int iiquestion_update(iiquestion_presentation *, const iiquestion_request_data *, size_t,
                                               const char *error, int *changed);
IIMULTIMODALAGENT_EXPORT int iiquestion_change(iiquestion_presentation *, const char *id,
                                               const iiquestion_request_data *value, size_t index, int *changed);
IIMULTIMODALAGENT_EXPORT int iiquestion_set_error(iiquestion_presentation *, const char *, int *changed);
IIMULTIMODALAGENT_EXPORT int iiquestion_read(iiquestion_presentation *, iiquestion_read_callback, void *);
IIMULTIMODALAGENT_EXPORT int iiquestion_rows(iiquestion_presentation *, uint64_t *generation, size_t *count);
IIMULTIMODALAGENT_EXPORT int iiquestion_boolean(iiquestion_presentation *, int property, int *);
IIMULTIMODALAGENT_EXPORT int iiquestion_set_motion(iiquestion_presentation *, int);
IIMULTIMODALAGENT_EXPORT int iiquestion_text(iiquestion_presentation *, int property, char *, size_t, size_t *);
IIMULTIMODALAGENT_EXPORT int iiquestion_row_boolean(iiquestion_presentation *, uint64_t, size_t, int property, int *);
IIMULTIMODALAGENT_EXPORT int iiquestion_row_text(iiquestion_presentation *, uint64_t, size_t, int property, char *, size_t, size_t *);
IIMULTIMODALAGENT_EXPORT int iiquestion_options(iiquestion_presentation *, uint64_t, size_t, size_t *count);
IIMULTIMODALAGENT_EXPORT int iiquestion_option_boolean(iiquestion_presentation *, uint64_t, size_t, size_t, int property, int *);
IIMULTIMODALAGENT_EXPORT int iiquestion_option_text(iiquestion_presentation *, uint64_t, size_t, size_t, int property, char *, size_t, size_t *);
IIMULTIMODALAGENT_EXPORT int iiquestion_select(iiquestion_presentation *, uint64_t, size_t, size_t, int selected);
IIMULTIMODALAGENT_EXPORT int iiquestion_preview(iiquestion_presentation *, uint64_t, size_t, size_t);
IIMULTIMODALAGENT_EXPORT int iiquestion_edit(iiquestion_presentation *, uint64_t, size_t, int property, const char *);
IIMULTIMODALAGENT_EXPORT int iiquestion_command(iiquestion_presentation *, int command, int *accepted);
IIMULTIMODALAGENT_EXPORT int iiquestion_acquire_model(iiquestion_presentation *, iiquestion_model_lease **);
IIMULTIMODALAGENT_EXPORT int iiquestion_lease_model(iiquestion_model_lease *, void **);
IIMULTIMODALAGENT_EXPORT int iiquestion_release_model(iiquestion_model_lease *);

#ifdef __cplusplus
}
#endif
