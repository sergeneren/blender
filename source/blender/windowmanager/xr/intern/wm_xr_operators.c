/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR Operators
 *
 * Collection of XR-related operators.
 */

#include "BLI_kdopbvh.h"
#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_context.h"
#include "BKE_editmesh.h"
#include "BKE_global.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "ED_mesh.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_select_utils.h"
#include "ED_space_api.h"
#include "ED_transform_snap_object_context.h"
#include "ED_view3d.h"

#include "GHOST_Types.h"

#include "GPU_immediate.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "wm_xr_intern.h"

/* -------------------------------------------------------------------- */
/** \name Operator Callbacks
 * \{ */

/* op->poll */
static bool wm_xr_operator_sessionactive(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (WM_xr_session_is_ready(&wm->xr)) {
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name XR Session Toggle
 *
 * Toggles an XR session, creating an XR context if necessary.
 * \{ */

static void wm_xr_session_update_screen(Main *bmain, const wmXrData *xr_data)
{
  const bool session_exists = WM_xr_session_exists(xr_data);

  for (bScreen *screen = bmain->screens.first; screen; screen = screen->id.next) {
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      LISTBASE_FOREACH (SpaceLink *, slink, &area->spacedata) {
        if (slink->spacetype == SPACE_VIEW3D) {
          View3D *v3d = (View3D *)slink;

          if (v3d->flag & V3D_XR_SESSION_MIRROR) {
            ED_view3d_xr_mirror_update(area, v3d, session_exists);
          }

          if (session_exists) {
            wmWindowManager *wm = bmain->wm.first;
            const Scene *scene = WM_windows_scene_get_from_screen(wm, screen);

            ED_view3d_xr_shading_update(wm, v3d, scene);
          }
          /* Ensure no 3D View is tagged as session root. */
          else {
            v3d->runtime.flag &= ~V3D_RUNTIME_XR_SESSION_ROOT;
          }
        }
      }
    }
  }

  WM_main_add_notifier(NC_WM | ND_XR_DATA_CHANGED, NULL);
}

static void wm_xr_session_update_screen_on_exit_cb(const wmXrData *xr_data)
{
  /* Just use G_MAIN here, storing main isn't reliable enough on file read or exit. */
  wm_xr_session_update_screen(G_MAIN, xr_data);
}

static int wm_xr_session_toggle_exec(bContext *C, wmOperator *UNUSED(op))
{
  Main *bmain = CTX_data_main(C);
  wmWindowManager *wm = CTX_wm_manager(C);
  wmWindow *win = CTX_wm_window(C);
  View3D *v3d = CTX_wm_view3d(C);

  /* Lazy-create xr context - tries to dynlink to the runtime, reading active_runtime.json. */
  if (wm_xr_init(wm) == false) {
    return OPERATOR_CANCELLED;
  }

  v3d->runtime.flag |= V3D_RUNTIME_XR_SESSION_ROOT;
  wm_xr_session_toggle(C, wm, win, wm_xr_session_update_screen_on_exit_cb);
  wm_xr_session_update_screen(bmain, &wm->xr);

  WM_event_add_notifier(C, NC_WM | ND_XR_DATA_CHANGED, NULL);

  return OPERATOR_FINISHED;
}

static void WM_OT_xr_session_toggle(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Toggle VR Session";
  ot->idname = "WM_OT_xr_session_toggle";
  ot->description =
      "Open a view for use with virtual reality headsets, or close it if already "
      "opened";

  /* callbacks */
  ot->exec = wm_xr_session_toggle_exec;
  ot->poll = ED_operator_view3d_active;

  /* XXX INTERNAL just to hide it from the search menu by default, an Add-on will expose it in the
   * UI instead. Not meant as a permanent solution. */
  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name XR Raycast Select
 *
 * Casts a ray from an XR controller's pose and selects any hit geometry.
 * \{ */

typedef struct XrRaycastSelectData {
  float origin[3];
  float direction[3];
  float end[3];
  void *draw_handle;
} XrRaycastSelectData;

void wm_xr_select_raycast_draw(const bContext *UNUSED(C),
                               ARegion *UNUSED(region),
                               void *customdata)
{
  const XrRaycastSelectData *data = customdata;

  const eGPUDepthTest depth_test_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  GPU_line_width(2.0f);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  immBegin(GPU_PRIM_LINES, 2);
  immUniformColor4f(0.35f, 0.35f, 1.0f, 1.0f);
  immVertex3fv(pos, data->origin);
  immVertex3fv(pos, data->end);
  immEnd();

  immUnbindProgram();

  GPU_depth_test(depth_test_prev);
}

static void wm_xr_select_raycast_init(bContext *C, wmOperator *op)
{
  BLI_assert(op->customdata == NULL);

  op->customdata = MEM_callocN(sizeof(XrRaycastSelectData), __func__);

  ARegionType *art = WM_xr_surface_region_type_get();
  if (art) {
    ((XrRaycastSelectData *)op->customdata)->draw_handle = ED_region_draw_cb_activate(
        art, wm_xr_select_raycast_draw, op->customdata, REGION_DRAW_POST_VIEW);
  }
}

static void wm_xr_select_raycast_uninit(bContext *C, wmOperator *op)
{
  if (op->customdata) {
    ARegionType *art = WM_xr_surface_region_type_get();
    if (art) {
      ED_region_draw_cb_exit(art, ((XrRaycastSelectData *)op->customdata)->draw_handle);
    }

    MEM_freeN(op->customdata);
  }
}

typedef enum eXrSelectElem {
  XR_SEL_BASE = 0,
  XR_SEL_VERTEX = 1,
  XR_SEL_EDGE = 2,
  XR_SEL_FACE = 3,
} eXrSelectElem;

static void wm_xr_select_op_apply(void *elem,
                                  BMesh *bm,
                                  eXrSelectElem select_elem,
                                  eSelectOp select_op,
                                  bool *r_changed,
                                  bool *r_set)
{
  const bool selected_prev = (select_elem == XR_SEL_BASE) ?
                                 (((Base *)elem)->flag & BASE_SELECTED) != 0 :
                                 (((BMElem *)elem)->head.hflag & BM_ELEM_SELECT) != 0;

  if (selected_prev) {
    switch (select_op) {
      case SEL_OP_SUB:
      case SEL_OP_XOR: {
        switch (select_elem) {
          case XR_SEL_BASE: {
            ED_object_base_select((Base *)elem, BA_DESELECT);
            *r_changed = true;
            break;
          }
          case XR_SEL_VERTEX: {
            BM_vert_select_set(bm, (BMVert *)elem, false);
            *r_changed = true;
            break;
          }
          case XR_SEL_EDGE: {
            BM_edge_select_set(bm, (BMEdge *)elem, false);
            *r_changed = true;
            break;
          }
          case XR_SEL_FACE: {
            BM_face_select_set(bm, (BMFace *)elem, false);
            *r_changed = true;
            break;
          }
          default: {
            break;
          }
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  else {
    switch (select_op) {
      case SEL_OP_SET:
      case SEL_OP_ADD:
      case SEL_OP_XOR: {
        switch (select_elem) {
          case XR_SEL_BASE: {
            ED_object_base_select((Base *)elem, BA_SELECT);
            *r_changed = true;
            break;
          }
          case XR_SEL_VERTEX: {
            BM_vert_select_set(bm, (BMVert *)elem, true);
            *r_changed = true;
            break;
          }
          case XR_SEL_EDGE: {
            BM_edge_select_set(bm, (BMEdge *)elem, true);
            *r_changed = true;
            break;
          }
          case XR_SEL_FACE: {
            BM_face_select_set(bm, (BMFace *)elem, true);
            *r_changed = true;
            break;
          }
          default: {
            break;
          }
        }
      }
      default: {
        break;
      }
    }

    if (select_op == SEL_OP_SET) {
      *r_set = true;
    }
  }
}

static bool wm_xr_select_raycast(bContext *C,
                                 const float origin[3],
                                 const float direction[3],
                                 float *ray_dist,
                                 eSelectOp select_op,
                                 bool deselect_all)
{
  /* Uses same raycast method as Scene.ray_cast(). */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc;
  ED_view3d_viewcontext_init(C, &vc, depsgraph);

  float location[3];
  float normal[3];
  int index;
  Object *ob = NULL;
  float obmat[4][4];

  SnapObjectContext *sctx = ED_transform_snap_object_context_create(vc.scene, 0);

  ED_transform_snap_object_project_ray_ex(sctx,
                                          depsgraph,
                                          &(const struct SnapObjectParams){
                                              .snap_select = SNAP_ALL,
                                          },
                                          origin,
                                          direction,
                                          ray_dist,
                                          location,
                                          normal,
                                          &index,
                                          &ob,
                                          obmat);

  ED_transform_snap_object_context_destroy(sctx);

  /* Select. */
  bool hit = false;
  bool changed = false;

  if (ob && BKE_object_is_in_editmode(ob)) {
    vc.em = BKE_editmesh_from_object(ob);

    if (vc.em) {
      BMesh *bm = vc.em->bm;
      BMFace *f = NULL;
      BMEdge *e = NULL;
      BMVert *v = NULL;

      if (index != -1) {
        ToolSettings *ts = vc.scene->toolsettings;
        float co[3];
        f = BM_face_at_index(bm, index);

        if ((ts->selectmode & SCE_SELECT_VERTEX) != 0) {
          /* Find nearest vertex. */
          float dist_max = *ray_dist;
          float dist;
          BMLoop *l = f->l_first;
          for (int i = 0; i < f->len; ++i, l = l->next) {
            mul_v3_m4v3(co, obmat, l->v->co);
            if ((dist = len_manhattan_v3v3(location, co)) < dist_max) {
              v = l->v;
              dist_max = dist;
            }
          }
          if (v) {
            hit = true;
          }
        }
        if ((ts->selectmode & SCE_SELECT_EDGE) != 0) {
          /* Find nearest edge. */
          float dist_max = *ray_dist;
          float dist;
          BMLoop *l = f->l_first;
          for (int i = 0; i < f->len; ++i, l = l->next) {
            add_v3_v3v3(co, l->e->v1->co, l->e->v2->co);
            mul_v3_fl(co, 0.5f);
            mul_m4_v3(obmat, co);
            if ((dist = len_manhattan_v3v3(location, co)) < dist_max) {
              e = l->e;
              dist_max = dist;
            }
          }
          if (e) {
            hit = true;
          }
        }
        if ((ts->selectmode & SCE_SELECT_FACE) != 0) {
          hit = true;
        }
        else {
          f = NULL;
        }
      }

      if (!hit) {
        if (deselect_all && (select_op == SEL_OP_SET)) {
          changed = EDBM_mesh_deselect_all_multi(C);
        }
      }
      else {
        bool set_v = false;
        bool set_e = false;
        bool set_f = false;

        if (v) {
          wm_xr_select_op_apply(v, bm, XR_SEL_VERTEX, select_op, &changed, &set_v);
        }
        if (e) {
          wm_xr_select_op_apply(e, bm, XR_SEL_EDGE, select_op, &changed, &set_e);
        }
        if (f) {
          wm_xr_select_op_apply(f, bm, XR_SEL_FACE, select_op, &changed, &set_f);
        }

        if (set_v || set_e || set_f) {
          EDBM_mesh_deselect_all_multi(C);
          if (set_v) {
            BM_vert_select_set(bm, v, true);
          }
          if (set_e) {
            BM_edge_select_set(bm, e, true);
          }
          if (set_f) {
            BM_face_select_set(bm, f, true);
          }
        }
      }

      if (changed) {
        DEG_id_tag_update((ID *)vc.obedit->data, ID_RECALC_SELECT);
        WM_event_add_notifier(C, NC_GEOM | ND_SELECT, vc.obedit->data);
      }
    }
  }
  else {
    if (ob) {
      hit = true;
    }

    if (!hit) {
      if (deselect_all && (select_op == SEL_OP_SET)) {
        changed = object_deselect_all_except(vc.view_layer, NULL);
      }
    }
    else {
      Base *base = BKE_view_layer_base_find(vc.view_layer, DEG_get_original_object(ob));
      if (base && BASE_SELECTABLE(vc.v3d, base)) {
        bool set = false;
        wm_xr_select_op_apply(base, NULL, XR_SEL_BASE, select_op, &changed, &set);
        if (set) {
          object_deselect_all_except(vc.view_layer, base);
        }
      }
    }

    if (changed) {
      DEG_id_tag_update(&vc.scene->id, ID_RECALC_SELECT);
      WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, vc.scene);
    }
  }

  return changed;
}

static int wm_xr_select_raycast_invoke_3d(bContext *C, wmOperator *op, const wmEvent *event)
{
  BLI_assert(event->type == EVT_XR_ACTION);
  BLI_assert(event->custom == EVT_DATA_XR);
  BLI_assert(event->customdata);

  wm_xr_select_raycast_init(C, op);

  int retval = op->type->modal_3d(C, op, event);

  if ((retval & OPERATOR_RUNNING_MODAL) != 0) {
    WM_event_add_modal_handler(C, op);
  }

  return retval;
}

static int wm_xr_select_raycast_exec(bContext *UNUSED(C), wmOperator *UNUSED(op))
{
  return OPERATOR_CANCELLED;
}

static int wm_xr_select_raycast_modal_3d(bContext *C, wmOperator *op, const wmEvent *event)
{
  BLI_assert(event->type == EVT_XR_ACTION);
  BLI_assert(event->custom == EVT_DATA_XR);
  BLI_assert(event->customdata);

  wmWindowManager *wm = CTX_wm_manager(C);
  wmXrActionData *actiondata = event->customdata;
  XrRaycastSelectData *data = op->customdata;

#if 1
  /* Use the "grip" pose forward axis. */
  float axis[3] = {0.0f, 0.0f, -1.0f};
#else
  /* Use the "aim" pose forward axis. */
  float axis[3] = {0.0f, 1.0f, 0.0f};
#endif
  copy_v3_v3(data->origin, actiondata->controller_loc);

  mul_qt_v3(actiondata->controller_rot, axis);
  copy_v3_v3(data->direction, axis);

  mul_v3_v3fl(data->end, data->direction, wm->xr.session_settings.clip_end);
  add_v3_v3(data->end, data->origin);

  if (event->val == KM_PRESS) {
    return OPERATOR_RUNNING_MODAL;
  }
  else if (event->val == KM_RELEASE) {
    PropertyRNA *prop = NULL;
    float ray_dist;
    eSelectOp select_op = SEL_OP_SET;
    bool deselect_all;
    bool ret;

    prop = RNA_struct_find_property(op->ptr, "distance");
    ray_dist = prop ? RNA_property_float_get(op->ptr, prop) : BVH_RAYCAST_DIST_MAX;

    prop = RNA_struct_find_property(op->ptr, "extend");
    if (prop && RNA_property_boolean_get(op->ptr, prop)) {
      select_op = SEL_OP_ADD;
    }
    prop = RNA_struct_find_property(op->ptr, "deselect");
    if (prop && RNA_property_boolean_get(op->ptr, prop)) {
      select_op = SEL_OP_SUB;
    }
    prop = RNA_struct_find_property(op->ptr, "toggle");
    if (prop && RNA_property_boolean_get(op->ptr, prop)) {
      select_op = SEL_OP_XOR;
    }

    prop = RNA_struct_find_property(op->ptr, "deselect_all");
    deselect_all = prop ? RNA_property_boolean_get(op->ptr, prop) : false;

    ret = wm_xr_select_raycast(
        C, data->origin, data->direction, &ray_dist, select_op, deselect_all);

    wm_xr_select_raycast_uninit(C, op);

    return ret ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }

  /* XR events currently only support press and release. */
  BLI_assert(false);
  wm_xr_select_raycast_uninit(C, op);
  return OPERATOR_CANCELLED;
}

static void WM_OT_xr_select_raycast(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "XR Raycast Select";
  ot->idname = "WM_OT_xr_select_raycast";
  ot->description = "Raycast select with a VR controller";

  /* callbacks */
  ot->invoke_3d = wm_xr_select_raycast_invoke_3d;
  ot->exec = wm_xr_select_raycast_exec;
  ot->modal_3d = wm_xr_select_raycast_modal_3d;
  ot->poll = wm_xr_operator_sessionactive;

  /* flags */
  ot->flag = OPTYPE_UNDO;

  /* properties */
  WM_operator_properties_mouse_select(ot);

  RNA_def_float(ot->srna,
                "distance",
                BVH_RAYCAST_DIST_MAX,
                0.0,
                BVH_RAYCAST_DIST_MAX,
                "",
                "Maximum distance",
                0.0,
                BVH_RAYCAST_DIST_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name XR Constraints Toggle
 *
 * Toggles enabled / auto key behavior for XR constraint objects.
 * \{ */

static void wm_xr_constraint_toggle(char *flag, bool enable, bool autokey)
{
  if (enable) {
    if ((*flag & XR_OBJECT_ENABLE) != 0) {
      *flag &= ~(XR_OBJECT_ENABLE);
    }
    else {
      *flag |= XR_OBJECT_ENABLE;
    }
  }

  if (autokey) {
    if ((*flag & XR_OBJECT_AUTOKEY) != 0) {
      *flag &= ~(XR_OBJECT_AUTOKEY);
    }
    else {
      *flag |= XR_OBJECT_AUTOKEY;
    }
  }
}

static int wm_xr_constraints_toggle_exec(bContext *C, wmOperator *op)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  XrSessionSettings *settings = &wm->xr.session_settings;
  PropertyRNA *prop = NULL;
  bool headset, controller0, controller1, enable, autokey;

  prop = RNA_struct_find_property(op->ptr, "headset");
  headset = prop ? RNA_property_boolean_get(op->ptr, prop) : true;

  prop = RNA_struct_find_property(op->ptr, "controller0");
  controller0 = prop ? RNA_property_boolean_get(op->ptr, prop) : true;

  prop = RNA_struct_find_property(op->ptr, "controller1");
  controller1 = prop ? RNA_property_boolean_get(op->ptr, prop) : true;

  prop = RNA_struct_find_property(op->ptr, "enable");
  enable = prop ? RNA_property_boolean_get(op->ptr, prop) : true;

  prop = RNA_struct_find_property(op->ptr, "autokey");
  autokey = prop ? RNA_property_boolean_get(op->ptr, prop) : true;

  if (headset) {
    wm_xr_constraint_toggle(&settings->headset_flag, enable, autokey);
  }
  if (controller0) {
    wm_xr_constraint_toggle(&settings->controller0_flag, enable, autokey);
  }
  if (controller1) {
    wm_xr_constraint_toggle(&settings->controller1_flag, enable, autokey);
  }

  WM_event_add_notifier(C, NC_WM | ND_XR_DATA_CHANGED, NULL);

  return OPERATOR_FINISHED;
}

static void WM_OT_xr_constraints_toggle(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "XR Constraints Toggle";
  ot->idname = "WM_OT_xr_constraints_toggle";
  ot->description = "Toggles enabled / auto key behavior for VR constraint objects";

  /* callbacks */
  ot->exec = wm_xr_constraints_toggle_exec;
  ot->poll = wm_xr_operator_sessionactive;

  /* properties */
  RNA_def_boolean(ot->srna, "headset", true, "Headset", "Toggle behavior for the headset object");
  RNA_def_boolean(ot->srna,
                  "controller0",
                  true,
                  "Controller 0",
                  "Toggle behavior for the first controller object ");
  RNA_def_boolean(ot->srna,
                  "controller1",
                  true,
                  "Controller 1",
                  "Toggle behavior for the second controller object");
  RNA_def_boolean(ot->srna, "enable", true, "Enable", "Toggle constraint enabled behavior");
  RNA_def_boolean(ot->srna, "autokey", false, "Auto Key", "Toggle auto keying behavior");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Registration
 * \{ */

void wm_xr_operatortypes_register(void)
{
  WM_operatortype_append(WM_OT_xr_session_toggle);
  WM_operatortype_append(WM_OT_xr_select_raycast);
  WM_operatortype_append(WM_OT_xr_constraints_toggle);
}

/** \} */