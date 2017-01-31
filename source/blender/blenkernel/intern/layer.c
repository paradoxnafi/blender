/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 *
 * Contributor(s): Dalai Felinto
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/layer.c
 *  \ingroup bke
 */

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"
#include "BLT_translation.h"

#include "BKE_layer.h"
#include "BKE_collection.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_node.h"

#include "DNA_ID.h"
#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

/* prototype */
static void layer_collection_free(SceneLayer *sl, LayerCollection *lc);
static LayerCollection *layer_collection_add(SceneLayer *sl, ListBase *lb, SceneCollection *sc);
static LayerCollection *find_layer_collection_by_scene_collection(LayerCollection *lc, const SceneCollection *sc);
static void object_bases_Iterator_next(Iterator *iter, const int flag);

/* RenderLayer */

/**
 * Add a new renderlayer
 * by default, a renderlayer has the master collection
 */
SceneLayer *BKE_scene_layer_add(Scene *scene, const char *name)
{
	if (!name) {
		name = DATA_("Render Layer");
	}

	SceneLayer *sl = MEM_callocN(sizeof(SceneLayer), "Scene Layer");
	sl->flag |= SCENE_LAYER_RENDER;

	BLI_addtail(&scene->render_layers, sl);

	/* unique name */
	BLI_strncpy_utf8(sl->name, name, sizeof(sl->name));
	BLI_uniquename(&scene->render_layers, sl, DATA_("SceneLayer"), '.', offsetof(SceneLayer, name), sizeof(sl->name));

	SceneCollection *sc = BKE_collection_master(scene);
	layer_collection_add(sl, &sl->layer_collections, sc);

	return sl;
}

bool BKE_scene_layer_remove(Main *bmain, Scene *scene, SceneLayer *sl)
{
	const int act = BLI_findindex(&scene->render_layers, sl);

	if (act == -1) {
		return false;
	}
	else if ( (scene->render_layers.first == scene->render_layers.last) &&
	          (scene->render_layers.first == sl))
	{
		/* ensure 1 layer is kept */
		return false;
	}

	BLI_remlink(&scene->render_layers, sl);

	BKE_scene_layer_free(sl);
	MEM_freeN(sl);

	scene->active_layer = 0;
	/* TODO WORKSPACE: set active_layer to 0 */

	for (Scene *sce = bmain->scene.first; sce; sce = sce->id.next) {
		if (sce->nodetree) {
			BKE_nodetree_remove_layer_n(sce->nodetree, scene, act);
		}
	}

	return true;
}

/**
 * Free (or release) any data used by this SceneLayer (does not free the SceneLayer itself).
 */
void BKE_scene_layer_free(SceneLayer *sl)
{
	sl->basact = NULL;
	BLI_freelistN(&sl->object_bases);

	for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
		layer_collection_free(NULL, lc);
	}
	BLI_freelistN(&sl->layer_collections);
}

/**
 * Set the render engine of a renderlayer
 */
void BKE_scene_layer_engine_set(SceneLayer *sl, const char *engine)
{
	BLI_strncpy_utf8(sl->engine, engine, sizeof(sl->engine));
}

/**
 * Tag all the selected objects of a renderlayer
 */
void BKE_scene_layer_selected_objects_tag(SceneLayer *sl, const int tag)
{
	for (ObjectBase *base = sl->object_bases.first; base; base = base->next) {
		if ((base->flag & BASE_SELECTED) != 0) {
			base->object->flag |= tag;
		}
		else {
			base->object->flag &= ~tag;
		}
	}
}

static bool find_scene_collection_in_scene_collections(ListBase *lb, const LayerCollection *lc)
{
	for (LayerCollection *lcn = lb->first; lcn; lcn = lcn->next) {
		if (lcn == lc) {
			return true;
		}
		if (find_scene_collection_in_scene_collections(&lcn->layer_collections, lc)) {
			return true;
		}
	}
	return false;
}

/**
 * Find the SceneLayer a LayerCollection belongs to
 */
SceneLayer *BKE_scene_layer_find_from_collection(Scene *scene, LayerCollection *lc)
{
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		if (find_scene_collection_in_scene_collections(&sl->layer_collections, lc)) {
			return sl;
		}
	}
	return false;
}

/* ObjectBase */

ObjectBase *BKE_scene_layer_base_find(SceneLayer *sl, Object *ob)
{
	return BLI_findptr(&sl->object_bases, ob, offsetof(ObjectBase, object));
}

void BKE_scene_layer_base_deselect_all(SceneLayer *sl)
{
	ObjectBase *base;

	for (base = sl->object_bases.first; base; base = base->next) {
		base->flag &= ~BASE_SELECTED;
	}
}

void BKE_scene_layer_base_select(struct SceneLayer *sl, ObjectBase *selbase)
{
	sl->basact = selbase;
	if ((selbase->flag & BASE_SELECTABLED) != 0) {
		selbase->flag |= BASE_SELECTED;
	}
}

static void scene_layer_object_base_unref(SceneLayer* sl, ObjectBase *base)
{
	base->refcount--;

	/* It only exists in the RenderLayer */
	if (base->refcount == 0) {
		if (sl->basact == base) {
			sl->basact = NULL;
		}

		BLI_remlink(&sl->object_bases, base);
		MEM_freeN(base);
	}
}

static void layer_collection_base_flag_recalculate(LayerCollection *lc, const bool tree_is_visible, const bool tree_is_selectable)
{
	bool is_visible = tree_is_visible && ((lc->flag & COLLECTION_VISIBLE) != 0);
	/* an object can only be selected if it's visible */
	bool is_selectable = tree_is_selectable && is_visible && ((lc->flag & COLLECTION_SELECTABLE) != 0);

	for (LinkData *link = lc->object_bases.first; link; link = link->next) {
		ObjectBase *base = link->data;

		if (is_visible) {
			base->flag |= BASE_VISIBLED;
		}
		else {
			base->flag &= ~BASE_VISIBLED;
		}

		if (is_selectable) {
			base->flag |= BASE_SELECTABLED;
		}
		else {
			base->flag &= ~BASE_SELECTABLED;
		}
	}

	for (LayerCollection *lcn = lc->layer_collections.first; lcn; lcn = lcn->next) {
		layer_collection_base_flag_recalculate(lcn, is_visible, is_selectable);
	}
}

/**
 * Re-evaluate the ObjectBase flags for SceneLayer
 */
void BKE_scene_layer_base_flag_recalculate(SceneLayer *sl)
{
	for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
		layer_collection_base_flag_recalculate(lc, true, true);
	}

	/* if base is not selectabled, clear select */
	for (ObjectBase *base = sl->object_bases.first; base; base = base->next) {
		if ((base->flag & BASE_SELECTABLED) == 0) {
			base->flag &= ~BASE_SELECTED;
		}
	}
}

/**
 * Return the base if existent, or create it if necessary
 * Always bump the refcount
 */
static ObjectBase *object_base_add(SceneLayer *sl, Object *ob)
{
	ObjectBase *base;
	base = BKE_scene_layer_base_find(sl, ob);

	if (base == NULL) {
		base = MEM_callocN(sizeof(ObjectBase), "Object Base");

		/* do not bump user count, leave it for SceneCollections */
		base->object = ob;
		BLI_addtail(&sl->object_bases, base);
	}
	base->refcount++;
	return base;
}

/* LayerCollection */

/**
 * When freeing the entire SceneLayer at once we don't bother with unref
 * otherwise SceneLayer is passed to keep the syncing of the LayerCollection tree
 */
static void layer_collection_free(SceneLayer *sl, LayerCollection *lc)
{
	if (sl) {
		for (LinkData *link = lc->object_bases.first; link; link = link->next) {
			scene_layer_object_base_unref(sl, link->data);
		}
	}

	BLI_freelistN(&lc->object_bases);
	BLI_freelistN(&lc->overrides);

	for (LayerCollection *nlc = lc->layer_collections.first; nlc; nlc = nlc->next) {
		layer_collection_free(sl, nlc);
	}
	BLI_freelistN(&lc->layer_collections);
}

/**
 * Free (or release) LayerCollection from SceneLayer
 * (does not free the LayerCollection itself).
 */
void BKE_layer_collection_free(SceneLayer *sl, LayerCollection *lc)
{
	layer_collection_free(sl, lc);
}

/* LayerCollection */

/**
 * Recursively get the collection for a given index
 */
static LayerCollection *collection_from_index(ListBase *lb, const int number, int *i)
{
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		if (*i == number) {
			return lc;
		}

		(*i)++;

		LayerCollection *lc_nested = collection_from_index(&lc->layer_collections, number, i);
		if (lc_nested) {
			return lc_nested;
		}
	}
	return NULL;
}

/**
 * Get the active collection
 */
LayerCollection *BKE_layer_collection_active(SceneLayer *sl)
{
	int i = 0;
	return collection_from_index(&sl->layer_collections, sl->active_collection, &i);
}

/**
 * Recursively get the count of collections
 */
static int collection_count(ListBase *lb)
{
	int i = 0;
	for (LayerCollection *lc = lb->first; lc; lc = lc->next) {
		i += collection_count(&lc->layer_collections) + 1;
	}
	return i;
}

/**
 * Get the total number of collections
 * (including all the nested collections)
 */
int BKE_layer_collection_count(SceneLayer *sl)
{
	return collection_count(&sl->layer_collections);
}

/**
 * Recursively get the index for a given collection
 */
static int index_from_collection(ListBase *lb, LayerCollection *lc, int *i)
{
	for (LayerCollection *lcol = lb->first; lcol; lcol = lcol->next) {
		if (lcol == lc) {
			return *i;
		}

		(*i)++;

		int i_nested = index_from_collection(&lcol->layer_collections, lc, i);
		if (i_nested != -1) {
			return i_nested;
		}
	}
	return -1;
}

/**
 * Return -1 if not found
 */
int BKE_layer_collection_findindex(SceneLayer *sl, LayerCollection *lc)
{
	int i = 0;
	return index_from_collection(&sl->layer_collections, lc, &i);
}

/**
 * Link a collection to a renderlayer
 * The collection needs to be created separately
 */
LayerCollection *BKE_collection_link(SceneLayer *sl, SceneCollection *sc)
{
	LayerCollection *lc = layer_collection_add(sl, &sl->layer_collections, sc);
	sl->active_collection = BKE_layer_collection_findindex(sl, lc);
	return lc;
}

/**
 * Unlink a collection base from a renderlayer
 * The corresponding collection is not removed from the master collection
 */
void BKE_collection_unlink(SceneLayer *sl, LayerCollection *lc)
{
	BKE_layer_collection_free(sl, lc);
	BKE_scene_layer_base_flag_recalculate(sl);

	BLI_remlink(&sl->layer_collections, lc);
	MEM_freeN(lc);
	sl->active_collection = 0;
}

static void layer_collection_object_add(SceneLayer *sl, LayerCollection *lc, Object *ob)
{
	ObjectBase *base = object_base_add(sl, ob);

	/* only add an object once - prevent SceneCollection->objects and
	 * SceneCollection->filter_objects to add the same object */

	if (BLI_findptr(&lc->object_bases, base, offsetof(LinkData, data))) {
		return;
	}

	BKE_scene_layer_base_flag_recalculate(sl);

	BLI_addtail(&lc->object_bases, BLI_genericNodeN(base));
}

static void layer_collection_object_remove(SceneLayer *sl, LayerCollection *lc, Object *ob)
{
	ObjectBase *base;
	base = BKE_scene_layer_base_find(sl, ob);

	LinkData *link = BLI_findptr(&lc->object_bases, base, offsetof(LinkData, data));
	BLI_remlink(&lc->object_bases, link);
	MEM_freeN(link);

	scene_layer_object_base_unref(sl, base);
}

static void layer_collection_objects_populate(SceneLayer *sl, LayerCollection *lc, ListBase *objects)
{
	for (LinkData *link = objects->first; link; link = link->next) {
		layer_collection_object_add(sl, lc, link->data);
	}
}

static void layer_collection_populate(SceneLayer *sl, LayerCollection *lc, SceneCollection *sc)
{
	layer_collection_objects_populate(sl, lc, &sc->objects);
	layer_collection_objects_populate(sl, lc, &sc->filter_objects);

	for (SceneCollection *nsc = sc->scene_collections.first; nsc; nsc = nsc->next) {
		layer_collection_add(sl, &lc->layer_collections, nsc);
	}
}

static LayerCollection *layer_collection_add(SceneLayer *sl, ListBase *lb, SceneCollection *sc)
{
	LayerCollection *lc = MEM_callocN(sizeof(LayerCollection), "Collection Base");
	BLI_addtail(lb, lc);

	lc->scene_collection = sc;
	lc->flag = COLLECTION_VISIBLE + COLLECTION_SELECTABLE + COLLECTION_FOLDED;

	layer_collection_populate(sl, lc, sc);
	return lc;
}


/* ---------------------------------------------------------------------- */

/**
 * See if render layer has the scene collection linked directly, or indirectly (nested)
 */
bool BKE_scene_layer_has_collection(struct SceneLayer *sl, struct SceneCollection *sc)
{
	for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
		if (find_layer_collection_by_scene_collection(lc, sc) != NULL) {
			return true;
		}
	}
	return false;
}

/**
 * See if the object is in any of the scene layers of the scene
 */
bool BKE_scene_has_object(Scene *scene, Object *ob)
{
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		ObjectBase *base = BKE_scene_layer_base_find(sl, ob);
		if (base) {
			return true;
		}
	}
	return false;
}


/* ---------------------------------------------------------------------- */
/* Syncing */

static LayerCollection *find_layer_collection_by_scene_collection(LayerCollection *lc, const SceneCollection *sc)
{
	if (lc->scene_collection == sc) {
		return lc;
	}

	for (LayerCollection *nlc = lc->layer_collections.first; nlc; nlc = nlc->next) {
		LayerCollection *found = find_layer_collection_by_scene_collection(nlc, sc);
		if (found) {
			return found;
		}
	}
	return NULL;
}

/**
 * Add a new LayerCollection for all the SceneLayers that have sc_parent
 */
void BKE_layer_sync_new_scene_collection(Scene *scene, const SceneCollection *sc_parent, SceneCollection *sc)
{
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
			LayerCollection *lc_parent = find_layer_collection_by_scene_collection(lc, sc_parent);
			if (lc_parent) {
				layer_collection_add(sl, &lc_parent->layer_collections, sc);
			}
		}
	}
}

/**
 * Add a corresponding ObjectBase to all the equivalent LayerCollection
 */
void BKE_layer_sync_object_link(Scene *scene, SceneCollection *sc, Object *ob)
{
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
			LayerCollection *found = find_layer_collection_by_scene_collection(lc, sc);
			if (found) {
				layer_collection_object_add(sl, found, ob);
			}
		}
	}
}

/**
 * Remove the equivalent object base to all layers that have this collection
 * also remove all reference to ob in the filter_objects
 */
void BKE_layer_sync_object_unlink(Scene *scene, SceneCollection *sc, Object *ob)
{
	for (SceneLayer *sl = scene->render_layers.first; sl; sl = sl->next) {
		for (LayerCollection *lc = sl->layer_collections.first; lc; lc = lc->next) {
			LayerCollection *found = find_layer_collection_by_scene_collection(lc, sc);
			if (found) {
				layer_collection_object_remove(sl, found, ob);
			}
		}
		BKE_scene_layer_base_flag_recalculate(sl);
	}
}

/* ---------------------------------------------------------------------- */
/* Override */

/**
 * Add a new datablock override
 */
void BKE_collection_override_datablock_add(LayerCollection *UNUSED(lc), const char *UNUSED(data_path), ID *UNUSED(id))
{
	TODO_LAYER_OVERRIDE;
}

/* ---------------------------------------------------------------------- */
/* Iterators */

static void object_bases_Iterator_begin(Iterator *iter, void *data_in, const int flag)
{
	SceneLayer *sl = data_in;
	ObjectBase *base = sl->object_bases.first;

	/* when there are no objects */
	if (base ==  NULL) {
		iter->valid = false;
		return;
	}

	iter->valid = true;
	iter->data = base;

	if ((base->flag & flag) == 0) {
		object_bases_Iterator_next(iter, flag);
	}
	else {
		iter->current = base;
	}
}

static void object_bases_Iterator_next(Iterator *iter, const int flag)
{
	ObjectBase *base = ((ObjectBase *)iter->data)->next;

	while (base) {
		if ((base->flag & flag) != 0) {
			iter->current = base;
			iter->data = base;
			return;
		}
		base = base->next;
	}

	iter->current = NULL;
	iter->valid = false;
}

static void objects_Iterator_begin(Iterator *iter, void *data_in, const int flag)
{
	object_bases_Iterator_begin(iter, data_in, flag);

	if (iter->valid) {
		iter->current = ((ObjectBase *)iter->current)->object;
	}
}

static void objects_Iterator_next(Iterator *iter, const int flag)
{
	object_bases_Iterator_next(iter, flag);

	if (iter->valid) {
		iter->current = ((ObjectBase *)iter->current)->object;
	}
}

void BKE_selected_objects_Iterator_begin(Iterator *iter, void *data_in)
{
	objects_Iterator_begin(iter, data_in, BASE_SELECTED);
}

void BKE_selected_objects_Iterator_next(Iterator *iter)
{
	objects_Iterator_next(iter, BASE_SELECTED);
}

void BKE_selected_objects_Iterator_end(Iterator *UNUSED(iter))
{
	/* do nothing */
}

void BKE_visible_objects_Iterator_begin(Iterator *iter, void *data_in)
{
	objects_Iterator_begin(iter, data_in, BASE_VISIBLED);
}

void BKE_visible_objects_Iterator_next(Iterator *iter)
{
	objects_Iterator_next(iter, BASE_VISIBLED);
}

void BKE_visible_objects_Iterator_end(Iterator *UNUSED(iter))
{
	/* do nothing */
}

void BKE_visible_bases_Iterator_begin(Iterator *iter, void *data_in)
{
	object_bases_Iterator_begin(iter, data_in, BASE_VISIBLED);
}

void BKE_visible_bases_Iterator_next(Iterator *iter)
{
	object_bases_Iterator_next(iter, BASE_VISIBLED);
}

void BKE_visible_bases_Iterator_end(Iterator *UNUSED(iter))
{
	/* do nothing */
}
