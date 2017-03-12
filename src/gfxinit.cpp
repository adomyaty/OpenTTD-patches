/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file gfxinit.cpp Initializing of the (GRF) graphics. */

#include "stdafx.h"
#include "fios.h"
#include "newgrf.h"
#include "3rdparty/md5/md5.h"
#include "font.h"
#include "gfx_func.h"
#include "transparency.h"
#include "blitter/blitter.h"
#include "video/video_driver.hpp"
#include "window_func.h"
#include "base_media_func.h"

/* The type of set we're replacing */
const char GraphicsSet::set_type[] = "graphics";

const char GraphicsSet::extension[] = ".obg"; // OpenTTD Base Graphics

#include "table/sprites.h"

/** Whether the given NewGRFs must get a palette remap from windows to DOS or not. */
bool _palette_remap_grf[MAX_FILE_SLOTS];

#include "table/landscape_sprite.h"

/** Offsets for loading the different "replacement" sprites in the files. */
static const SpriteID * const _landscape_spriteindexes[] = {
	_landscape_spriteindexes_arctic,
	_landscape_spriteindexes_tropic,
	_landscape_spriteindexes_toyland,
};

/**
 * Load an old fashioned GRF file.
 * @param filename   The name of the file to open.
 * @param load_index The offset of the first sprite.
 * @param file_index The Fio offset to load the file in.
 * @return The number of loaded sprites.
 */
static uint LoadGrfFile(const char *filename, uint load_index, int file_index)
{
	uint load_index_org = load_index;
	uint sprite_id = 0;

	FioOpenFile(file_index, filename, BASESET_DIR);

	DEBUG(sprite, 2, "Reading grf-file '%s'", filename);

	byte container_ver = GetGRFContainerVersion();
	if (container_ver == 0) usererror("Base grf '%s' is corrupt", filename);
	ReadGRFSpriteOffsets(container_ver);
	if (container_ver >= 2) {
		/* Read compression. */
		byte compression = FioReadByte();
		if (compression != 0) usererror("Unsupported compression format");
	}

	while (LoadNextSprite(load_index, file_index, sprite_id, container_ver)) {
		load_index++;
		sprite_id++;
		if (load_index >= MAX_SPRITES) {
			usererror("Too many sprites. Recompile with higher MAX_SPRITES value or remove some custom GRF files.");
		}
	}
	DEBUG(sprite, 2, "Currently %i sprites are loaded", load_index);

	return load_index - load_index_org;
}

/**
 * Load an old fashioned GRF file to replace already loaded sprites.
 * @param filename   The name of the file to open.
 * @param index_tlb  The offsets of each of the sprites.
 * @param file_index The Fio offset to load the file in.
 * @return The number of loaded sprites.
 */
static void LoadGrfFileIndexed(const char *filename, const SpriteID *index_tbl, int file_index)
{
	uint start;
	uint sprite_id = 0;

	FioOpenFile(file_index, filename, BASESET_DIR);

	DEBUG(sprite, 2, "Reading indexed grf-file '%s'", filename);

	byte container_ver = GetGRFContainerVersion();
	if (container_ver == 0) usererror("Base grf '%s' is corrupt", filename);
	ReadGRFSpriteOffsets(container_ver);
	if (container_ver >= 2) {
		/* Read compression. */
		byte compression = FioReadByte();
		if (compression != 0) usererror("Unsupported compression format");
	}

	while ((start = *index_tbl++) != END) {
		uint end = *index_tbl++;

		do {
			bool b = LoadNextSprite(start, file_index, sprite_id, container_ver);
			assert(b);
			sprite_id++;
		} while (++start <= end);
	}
}

/**
 * Set the graphics set to be used.
 * @param name of the set to use
 * @return true if it could be loaded
 */
bool BaseGraphics::SetSet (const char *name)
{
	if (!BaseMedia<GraphicsSet>::SetSet (name)) return false;

	const GraphicsSet *used_set = BaseGraphics::GetUsedSet();
	if (used_set == NULL) return true;

	DEBUG(grf, 1, "Using the %s base graphics set", used_set->get_name());

	if (used_set->GetNumInvalid() != 0) {
		/* Not all files were loaded successfully, see which ones */
		sstring<1024> error_msg;
		for (uint i = 0; i < GraphicsSet::NUM_FILES; i++) {
			MD5File::ChecksumResult res = GraphicsSet::CheckMD5 (&used_set->files[i]);
			if (res != MD5File::CR_MATCH) error_msg.append_fmt ("\t%s is %s (%s)\n", used_set->files[i].filename, res == MD5File::CR_MISMATCH ? "corrupt" : "missing", used_set->files[i].missing_warning);
		}
		ShowInfoF ("Trying to load graphics set '%s', but it is incomplete. The game will probably not run correctly until you properly install this set or select another one. See section 4.1 of readme.txt.\n\nThe following files are corrupted or missing:\n%s", used_set->get_name(), error_msg.c_str());
	}

	return true;
}

/**
 * Set the sounds set to be used.
 * @param name of the set to use
 * @return true if it could be loaded
 */
bool BaseSounds::SetSet (const char *name)
{
	if (!BaseMedia<SoundsSet>::SetSet (name)) return false;

	const SoundsSet *sounds_set = BaseSounds::GetUsedSet();
	if (sounds_set == NULL) return true;

	if (sounds_set->GetNumInvalid() != 0) {
		assert_compile(SoundsSet::NUM_FILES == 1);
		/* No need to loop each file, as long as there is only a single
		 * sound file. */
		ShowInfoF ("Trying to load sound set '%s', but it is incomplete. The game will probably not run correctly until you properly install this set or select another one. See section 4.1 of readme.txt.\n\nThe following files are corrupted or missing:\n\t%s is %s (%s)\n",
				sounds_set->get_name(), sounds_set->files->filename,
				SoundsSet::CheckMD5 (sounds_set->files) == MD5File::CR_MISMATCH ? "corrupt" : "missing",
				sounds_set->files->missing_warning);
	}

	return true;
}

/** Actually load the sprite tables. */
static void LoadSpriteTables()
{
	memset(_palette_remap_grf, 0, sizeof(_palette_remap_grf));
	uint i = FIRST_GRF_SLOT;
	const GraphicsSet *used_set = BaseGraphics::GetUsedSet();

	_palette_remap_grf[i] = (PAL_DOS != used_set->palette);
	LoadGrfFile(used_set->files[GFT_BASE].filename, 0, i++);

	/*
	 * The second basic file always starts at the given location and does
	 * contain a different amount of sprites depending on the "type"; DOS
	 * has a few sprites less. However, we do not care about those missing
	 * sprites as they are not shown anyway (logos in intro game).
	 */
	_palette_remap_grf[i] = (PAL_DOS != used_set->palette);
	LoadGrfFile(used_set->files[GFT_LOGOS].filename, 4793, i++);

	/*
	 * Load additional sprites for climates other than temperate.
	 * This overwrites some of the temperate sprites, such as foundations
	 * and the ground sprites.
	 */
	if (_settings_game.game_creation.landscape != LT_TEMPERATE) {
		_palette_remap_grf[i] = (PAL_DOS != used_set->palette);
		LoadGrfFileIndexed(
			used_set->files[GFT_ARCTIC + _settings_game.game_creation.landscape - 1].filename,
			_landscape_spriteindexes[_settings_game.game_creation.landscape - 1],
			i++
		);
	}

	/* Initialize the unicode to sprite mapping table */
	InitializeUnicodeGlyphMap();

	/*
	 * Load the base NewGRF with OTTD required graphics as first NewGRF.
	 * However, we do not want it to show up in the list of used NewGRFs,
	 * so we have to manually add it, and then remove it later.
	 */
	GRFConfig *top = _grfconfig;
	GRFConfig *master = new GRFConfig(used_set->files[GFT_EXTRA].filename);

	/* We know the palette of the base set, so if the base NewGRF is not
	 * setting one, use the palette of the base set and not the global
	 * one which might be the wrong palette for this base NewGRF.
	 * The value set here might be overridden via action14 later. */
	switch (used_set->palette) {
		case PAL_DOS:     master->palette |= GRFP_GRF_DOS;     break;
		case PAL_WINDOWS: master->palette |= GRFP_GRF_WINDOWS; break;
		default: break;
	}
	FillGRFDetails(master, false, BASESET_DIR);

	ClrBit(master->flags, GCF_INIT_ONLY);
	master->next = top;
	_grfconfig = master;

	LoadNewGRF(SPR_NEWGRFS_BASE, i);

	/* Free and remove the top element. */
	delete master;
	_grfconfig = top;
}


/**
 * Select the blitter needed by NewGRF config.
 * @return The blitter to switch to.
 */
static const char *SelectNewGRFBlitter (void)
{
	/* Get preferred depth.
	 *  - base_wants_32bpp: Depth required by the baseset, i.e. the majority of the sprites.
	 *  - grf_wants_32bpp:  Depth required by some NewGRF.
	 * Both can force using a 32bpp blitter. base_wants_32bpp is used to select
	 * between multiple 32bpp blitters, which perform differently with 8bpp sprites.
	 */
	bool base_wants_32bpp = BaseGraphics::GetUsedSet()->blitter == BLT_32BPP;
	bool grf_wants_32bpp;
	if (_support8bpp == S8BPP_NONE) {
		grf_wants_32bpp = true;
	} else {
		grf_wants_32bpp = false;
		for (GRFConfig *c = _grfconfig; c != NULL; c = c->next) {
			if (c->status == GCS_DISABLED || c->status == GCS_NOT_FOUND || HasBit(c->flags, GCF_INIT_ONLY)) continue;
			if (c->palette & GRFP_BLT_32BPP) {
				grf_wants_32bpp = true;
				break;
			}
		}
	}

	/* Search the best blitter. */
	static const struct {
		const char *name;
		byte animation;   ///< 0: no support, 1: do support, 2: both
		byte base_depth;  ///< 0: 8bpp, 1: 32bpp, 2: both
		byte grf_depth;   ///< 0: 8bpp, 1: 32bpp, 2: both
	} replacement_blitters[] = {
#ifdef WITH_SSE
		{ "32bpp-sse4",       0,  1,  2 },
		{ "32bpp-ssse3",      0,  1,  2 },
		{ "32bpp-sse2",       0,  1,  2 },
		{ "32bpp-sse4-anim",  1,  1,  2 },
#endif
		{ "8bpp-optimized",   2,  0,  0 },
		{ "32bpp-optimized",  0,  2,  2 },
		{ "32bpp-anim",       1,  2,  2 },
	};

	const bool animation_wanted = HasBit(_display_opt, DO_FULL_ANIMATION);

	for (uint i = 0; ; i++) {
		/* One of the last two blitters should always match. */
		assert (i < lengthof(replacement_blitters));

		if (replacement_blitters[i].animation  == (animation_wanted ? 0 : 1)) continue;
		if (replacement_blitters[i].base_depth == (base_wants_32bpp ? 0 : 1)) continue;
		if (replacement_blitters[i].grf_depth  == (grf_wants_32bpp  ? 0 : 1)) continue;

		return replacement_blitters[i].name;
	}
}

/**
 * Check blitter needed by NewGRF config and switch if needed.
 * @return False when nothing changed, true otherwise.
 */
static bool SwitchNewGRFBlitter()
{
	/* Never switch if the blitter was specified by the user. */
	if (!Blitter::autodetected) return false;

	/* Null driver => dedicated server => do nothing. */
	if (Blitter::get()->GetScreenDepth() == 0) return false;

	const char *repl_blitter = SelectNewGRFBlitter();
	const char *cur_blitter = Blitter::get_name();
	if (strcmp (repl_blitter, cur_blitter) == 0) return false;

	DEBUG(misc, 1, "Switching blitter from '%s' to '%s'... ", cur_blitter, repl_blitter);
	Blitter *new_blitter = Blitter::select (repl_blitter);
	/* Blitter::select only fails if it cannot find a blitter by
	 * the given name, and all of the replacement blitters in the
	 * replacement list should be available. */
	assert (new_blitter != NULL);
	DEBUG(misc, 1, "Successfully switched to %s.", repl_blitter);

	if (!VideoDriver::GetActiveDriver()->AfterBlitterChange()) {
		/* Failed to switch blitter, let's hope we can return to the old one. */
		if (Blitter::select (cur_blitter) == NULL || !VideoDriver::GetActiveDriver()->AfterBlitterChange()) {
			usererror("Failed to reinitialize video driver. Specify a fixed blitter in the config");
		}
	}

	return true;
}

/** Check whether we still use the right blitter, or use another (better) one. */
void CheckBlitter()
{
	if (!SwitchNewGRFBlitter()) return;

	ClearFontCache();
	GfxClearSpriteCache();
	ReInitAllWindows();
}

/** Initialise and load all the sprites. */
void GfxLoadSprites()
{
	DEBUG(sprite, 2, "Loading sprite set %d", _settings_game.game_creation.landscape);

	SwitchNewGRFBlitter();
	ClearFontCache();
	GfxInitSpriteMem();
	LoadSpriteTables();
	GfxInitPalettes();

	UpdateCursorSize();
}

bool GraphicsSet::FillSetDetails(IniFile *ini, const char *path, const char *full_filename)
{
	bool ret = this->BaseSet<GraphicsSet, MAX_GFT>::FillSetDetails (ini, path, full_filename, false);
	if (ret) {
		const IniGroup *metadata = ini->get_group ("metadata");
		const IniItem *item;

		item = this->fetch_metadata (metadata, "palette", full_filename);
		if (item == NULL) return false;
		this->palette = (*item->value == 'D' || *item->value == 'd') ? PAL_DOS : PAL_WINDOWS;

		/* Get optional blitter information. */
		item = metadata->find ("blitter");
		this->blitter = (item != NULL && *item->value == '3') ? BLT_32BPP : BLT_8BPP;
	}
	return ret;
}

/**
 * Calculate and check the MD5 hash of the supplied file.
 * @param f The file to check.
 * @param hash The hash to check against.
 * @param size Use only this many bytes from the file.
 * @return Whether the file matches the given hash.
 */
static bool check_md5 (FILE *f, const byte (&hash) [16], size_t size)
{
	Md5 checksum;
	byte buffer[1024];

	while (size != 0) {
		size_t len = fread (buffer, 1, min (size, sizeof(buffer)), f);
		if (len == 0) break;
		size -= len;
		checksum.Append (buffer, len);
	}

	FioFCloseFile(f);

	byte digest[16];
	checksum.Finish (digest);
	return memcmp (hash, digest, sizeof(hash)) == 0;
}

/**
 * Calculate and check the MD5 hash of the supplied GRF.
 * @param file The file get the hash of.
 * @return
 * - #CR_MATCH if the MD5 hash matches
 * - #CR_MISMATCH if the MD5 does not match
 * - #CR_NO_FILE if the file misses
 */
/* static */ MD5File::ChecksumResult GraphicsSet::CheckMD5 (const MD5File *file)
{
	size_t size = 0;
	FILE *f = FioFOpenFile (file->filename, "rb", BASESET_DIR, &size);
	if (f == NULL) return MD5File::CR_NO_FILE;

	size = min (size, GRFGetSizeOfDataSection (f));

	fseek (f, 0, SEEK_SET);

	return check_md5 (f, file->hash, size) ?
			MD5File::CR_MATCH : MD5File::CR_MISMATCH;
}


/**
 * Calculate and check the MD5 hash of the supplied filename.
 * @return
 * - #CR_MATCH if the MD5 hash matches
 * - #CR_MISMATCH if the MD5 does not match
 * - #CR_NO_FILE if the file misses
 */
MD5File::ChecksumResult MD5File::CheckMD5 (void) const
{
	size_t size;
	FILE *f = FioFOpenFile (this->filename, "rb", BASESET_DIR, &size);

	if (f == NULL) return CR_NO_FILE;

	return check_md5 (f, this->hash, size) ? CR_MATCH : CR_MISMATCH;
}

/** Names corresponding to the GraphicsFileType */
const char * const GraphicsSet::file_names [MAX_GFT] =
	{ "base", "logos", "arctic", "tropical", "toyland", "extra" };

INSTANTIATE_BASE_MEDIA_METHODS(BaseMedia<GraphicsSet>, GraphicsSet)
