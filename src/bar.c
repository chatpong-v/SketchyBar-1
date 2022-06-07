#include "bar.h"
#include "bar_manager.h"
#include "event.h"
#include "event_loop.h"
#include "display.h"
#include "misc/helpers.h"
#include "window.h"

void bar_draw_graph(struct bar* bar, struct bar_item* bar_item, uint32_t x, bool right_to_left) {
  if (!bar_item->has_graph) return;
}

bool bar_draws_item(struct bar* bar, struct bar_item* bar_item) {
    if (!bar_item->drawing || !bar->shown) return false;
    if (bar_item->associated_display > 0
        && (!(bar_item->associated_display & (1 << bar->adid)))
            && !bar_item->ignore_association)
      return false;
    if (bar_item->associated_space > 0
        && (!(bar_item->associated_space & (1 << bar->sid))
            && !bar_item->ignore_association)
        && (bar_item->type != BAR_COMPONENT_SPACE)        )
      return false;
    if (bar_item->position == POSITION_POPUP)
      return false;
    return true;
}

void bar_calculate_popup_anchor_for_bar_item(struct bar* bar, struct bar_item* bar_item) {
  if (bar->adid != g_bar_manager.active_adid) return;
  struct window* window = bar_item_get_window(bar_item, bar->adid);
  if (!bar_item->popup.overrides_cell_size)
    bar_item->popup.cell_size = window->frame.size.height;

  bool needs_recalculation = bar_item->popup.adid != bar->adid;
  popup_calculate_bounds(&bar_item->popup);

  CGPoint anchor = window->origin;
  if (bar_item->popup.align == POSITION_CENTER) {
    anchor.x += (window->frame.size.width
                 - bar_item->popup.background.bounds.size.width) / 2;
  } else if (bar_item->popup.align == POSITION_LEFT) {
    anchor.x -= bar_item->background.padding_left;
  } else {
    anchor.x += window->frame.size.width
                - bar_item->popup.background.bounds.size.width;
  }
  anchor.y += (g_bar_manager.position == POSITION_BOTTOM
              ? (- bar_item->popup.background.bounds.size.height)
              : window->frame.size.height);

  popup_set_anchor(&bar_item->popup, anchor, bar->adid);
  if (needs_recalculation) {
    popup_calculate_bounds(&bar_item->popup);
  }
}

void bar_order_item_windows(struct bar* bar, int mode) {
  SLSOrderWindow(g_connection, bar->window.id, 1, 0);

  struct window* previous_window = NULL;
  for (int i = 0; i < g_bar_manager.bar_item_count; i++) {
    struct bar_item* bar_item = g_bar_manager.bar_items[i];

    struct window* window = bar_item_get_window(bar_item, bar->adid);
    SLSRemoveFromOrderingGroup(g_connection, window->id);
    window_set_level(window, g_bar_manager.window_level);

    if (bar_item->type == BAR_COMPONENT_GROUP) {
      SLSOrderWindow(g_connection, window->id, mode, bar->window.id);
      SLSAddWindowToWindowOrderingGroup(g_connection,
                                        bar->window.id,
                                        window->id,
                                        1              );
      continue;
    }

    if (previous_window) {
      SLSOrderWindow(g_connection, window->id, mode, previous_window->id);
      SLSAddWindowToWindowOrderingGroup(g_connection,
                                        previous_window->id,
                                        window->id,
                                        1                   );
    }
    else {
      SLSOrderWindow(g_connection, window->id, mode, bar->window.id);
      SLSAddWindowToWindowOrderingGroup(g_connection,
                                        bar->window.id,
                                        window->id,
                                        1              );
    }
    previous_window = window;
  }
}

void bar_draw(struct bar* bar) {
  if (bar->needs_update) {
    draw_rect(bar->window.context,
              bar->window.frame,
              &g_bar_manager.background.color,
              g_bar_manager.background.corner_radius,
              g_bar_manager.background.border_width,
              &g_bar_manager.background.border_color,
              true                                   );

    bar->needs_update = false;
    CGContextFlush(bar->window.context);
  }

  if (g_bar_manager.background.image.enabled) {
    image_draw(&g_bar_manager.background.image, bar->window.context);
  }

  for (int i = 0; i < g_bar_manager.bar_item_count; i++) {
    struct bar_item* bar_item = g_bar_manager.bar_items[i];

    if (bar_item->position == POSITION_POPUP) {
      continue;
    }

    struct window* window = bar_item_get_window(bar_item, bar->adid);

    bar_item_remove_associated_bar(bar_item, bar->adid);

    if (!bar_draws_item(bar, bar_item)
        || (bar_item->type == BAR_COMPONENT_GROUP
            && !bar_draws_item(bar, group_get_first_member(bar_item->group)))){

      if (!CGPointEqualToPoint(window->origin, g_nirvana)) {
        window->origin = g_nirvana;
        SLSMoveWindow(g_connection, window->id, &g_nirvana);
      }

      continue;
    }

    bar_item_append_associated_bar(bar_item, bar->adid);

    if (bar_item->popup.drawing && bar->adid == g_bar_manager.active_adid)
      popup_draw(&bar_item->popup);

    if (!window_apply_frame(window) && !bar_item->needs_update) continue;

    if (bar_item->update_mask & UPDATE_MOUSE_ENTERED
        || bar_item->update_mask & UPDATE_MOUSE_EXITED) {
      CGRect tracking_rect = window->frame;

      SLSRemoveAllTrackingAreas(g_connection, window->id);
      SLSAddTrackingRect(g_connection, window->id, tracking_rect);
    }

    CGContextClearRect(window->context, window->frame);

    bar_item_draw(bar_item, window->context);
    CGContextFlush(window->context);
  }
}

void bar_calculate_bounds(struct bar* bar) {
  if (bar->hidden) return;
  if (bar->sid == 0) return;
  uint32_t notch_width = CGDisplayIsBuiltin(bar->did)
                         ? g_bar_manager.notch_width
                         : 0;

  uint32_t center_length = bar_manager_length_for_bar_side(&g_bar_manager,
                                                           bar,
                                                           POSITION_CENTER);

  uint32_t bar_left_first_item_x = g_bar_manager.background.padding_left;
  uint32_t bar_right_first_item_x = bar->window.frame.size.width
                                    - g_bar_manager.background.padding_right;

  uint32_t bar_center_first_item_x = (bar->window.frame.size.width
                                      - 2*g_bar_manager.margin
                                      - center_length) / 2 - 1;

  uint32_t bar_center_right_first_item_x = (bar->window.frame.size.width
                                            + notch_width) / 2 - 1;

  uint32_t bar_center_left_first_item_x = (bar->window.frame.size.width
                                           - notch_width) / 2 - 1;

  uint32_t* next_position = NULL;
  uint32_t y = bar->window.frame.size.height / 2;

  for (int i = 0; i < g_bar_manager.bar_item_count; i++) {
    struct bar_item* bar_item = g_bar_manager.bar_items[i];

    if (!bar_draws_item(bar, bar_item)
        || bar_item->type == BAR_COMPONENT_GROUP) {
      continue;
    } 

    uint32_t bar_item_display_length = bar_item_get_length(bar_item, true);
    bool rtl = false;

    if (bar_item->position == POSITION_LEFT)
      next_position = &bar_left_first_item_x;
    else if (bar_item->position == POSITION_CENTER)
      next_position = &bar_center_first_item_x;
    else if (bar_item->position == POSITION_RIGHT)
      next_position = &bar_right_first_item_x, rtl = true;
    else if (bar_item->position == POSITION_CENTER_RIGHT)
      next_position = &bar_center_right_first_item_x;
    else if (bar_item->position == POSITION_CENTER_LEFT)
      next_position = &bar_center_left_first_item_x, rtl = true;
    else
      continue;

    if (bar_item->position == POSITION_RIGHT
        || bar_item->position == POSITION_CENTER_LEFT) {
      *next_position -= bar_item_display_length
                        + bar_item->background.padding_right;
    }
    else {
      *next_position += bar_item->background.padding_left;
    }

    bar_item->graph.rtl = rtl;
    uint32_t bar_item_length = bar_item_calculate_bounds(bar_item,
                                 bar->window.frame.size.height
                                 - (g_bar_manager.background.border_width + 1),
                                 0,
                                 y                                           );

    CGRect frame = {{bar->window.origin.x + *next_position,
                     bar->window.origin.y                  },
                    {bar_item_display_length,
                     bar->window.frame.size.height}          };

    window_set_frame(bar_item_get_window(bar_item, bar->adid), frame);

    if (bar_item->group && group_is_first_member(bar_item->group, bar_item)) {
      uint32_t group_length = group_get_length(bar_item->group);
      uint32_t group_offset = (bar_item->position == POSITION_RIGHT
                               || bar_item->position == POSITION_CENTER_LEFT)
                              ? group_length
                                - bar_item_get_length(bar_item, false)
                                - group_count_members_drawn(bar_item->group)
                              : 0;

      CGRect group_frame = {{frame.origin.x - group_offset,
                             frame.origin.y                        },
                            {group_length,
                             frame.size.height}                      };
      
      window_set_frame(bar_item_get_window(bar_item->group->members[0],
                                           bar->adid                   ),
                       group_frame                                       );
    }

    if (bar_item->popup.drawing)
      bar_calculate_popup_anchor_for_bar_item(bar, bar_item);

    if (bar_item->position == POSITION_RIGHT
        || bar_item->position == POSITION_CENTER_LEFT) {
      *next_position += bar_item->has_const_width
                        ? bar_item_display_length
                          + bar_item->background.padding_right
                          - bar_item->custom_width
                        : - bar_item->background.padding_left;
    } else {
      *next_position += bar_item->has_const_width
                        ? bar_item->custom_width
                          - bar_item->background.padding_left
                        : (bar_item_length
                           + bar_item->background.padding_right);
    }
  }
}

CGRect bar_get_frame(struct bar *bar) {
  CGRect bounds = display_bounds(bar->did);
  bounds.size.width -= 2*g_bar_manager.margin;
  CGPoint origin = bounds.origin;
  origin.x += g_bar_manager.margin;
  origin.y += g_bar_manager.y_offset;


  if (g_bar_manager.position == POSITION_BOTTOM) {
    origin.y = CGRectGetMaxY(bounds)
               - g_bar_manager.background.bounds.size.height
               - 2*g_bar_manager.y_offset;
  } else if (display_menu_bar_visible() && !g_bar_manager.topmost) {
    CGRect menu = display_menu_bar_rect(bar->did);
    origin.y += menu.size.height;
  }

  return (CGRect) {{origin.x, origin.y},{bounds.size.width,
                                 g_bar_manager.background.bounds.size.height}};
}

void bar_resize(struct bar* bar) {
  if (bar->hidden) return;
  window_set_frame(&bar->window, bar_get_frame(bar));
  window_apply_frame(&bar->window);
  SLSRemoveAllTrackingAreas(g_connection, bar->window.id);
  SLSAddTrackingRect(g_connection, bar->window.id, bar->window.frame);
  bar_calculate_bounds(bar);
  bar->needs_update = true;
  bar_draw(bar);
}

void bar_set_hidden(struct bar* bar, bool hidden) {
  if (bar->hidden == hidden) return;
  if (hidden) window_close(&bar->window);
  else bar_create_window(bar);
  bar->hidden = hidden;
}

void bar_create_window(struct bar* bar) {
  window_create(&bar->window, bar_get_frame(bar));
  SLSRemoveAllTrackingAreas(g_connection, bar->window.id);
  SLSAddTrackingRect(g_connection, bar->window.id, bar->window.frame);
  window_set_level(&bar->window, g_bar_manager.window_level);
  window_set_blur_radius(&bar->window, g_bar_manager.blur_radius);
  if (!g_bar_manager.shadow) window_disable_shadow(&bar->window);

  context_set_font_smoothing(bar->window.context, g_bar_manager.font_smoothing);
}

struct bar *bar_create(uint32_t did) {
  struct bar *bar = malloc(sizeof(struct bar));
  memset(bar, 0, sizeof(struct bar));
  bar->hidden = false;
  bar->did = did;
  bar->sid = mission_control_index(display_space_id(did));
  bar->shown = true;
  bar->needs_update = true;
  bar_create_window(bar);
  return bar;
}

void bar_destroy(struct bar *bar) {
  window_close(&bar->window);
  free(bar);
}
