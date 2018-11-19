/**
  * Touhou Community Reliant Automatic Patcher
  * BGM modding library for games using uncompressed PCM music
  *
  * ----
  *
  * Implementations of public functions not related to specific codecs or
  * streaming APIs.
  */

#include <thcrap.h>
#include "bgmmod.hpp"

/// String constants
/// ----------------
const stringref_t LOOP_INFIX = ".loop";
const int LONGEST_CODEC_LEN = [] {
	int ret = 0;
	for(const auto &codec : CODECS) {
		ret = max(ret, codec.ext.len);
	}
	return ret;
}();
/// ----------------

/// Sampling rate / bit depth / channel structure
/// ---------------------------------------------
void pcm_format_t::patch(WAVEFORMATEX &wfx) const
{
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = channels;
	wfx.nSamplesPerSec = samplingrate;
	wfx.wBitsPerSample = bitdepth;
	wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
}

void pcm_format_t::patch(PCMWAVEFORMAT &pwf) const
{
	patch((WAVEFORMATEX &)pwf);
}
/// ---------------------------------------------

/// Track class
/// -----------
void track_pcm_t::decode(void *_buf, size_t size)
{
	auto *buf = (uint8_t*)_buf;
	assert(buf);
	while(size > 0) {
		auto ret = cur->part_decode(buf, size);
		if(ret == (size_t)-1) {
			// TODO: Error?
			return;
		} else if(ret == 0) {
			if((cur == intro.get()) && (loop != nullptr)) {
				cur = loop.get();
			}
			cur->part_seek_to_sample(0);
		}
		buf += ret;
		size -= ret;
	}
}

void track_pcm_t::seek_to_byte(size_t byte)
{
	auto *actual_looping_segment = loop ? loop.get() : intro.get();
	if(byte < intro->part_bytes) {
		cur = intro.get();
	} else {
		cur = actual_looping_segment;
		auto loop_length = cur->part_bytes;
		byte -= intro->part_bytes;
		byte %= loop_length;
	}
	cur->part_seek_to_sample(byte / (pcmf.bitdepth / 8) / pcmf.channels);
}

std::unique_ptr<track_pcm_t> pcm_open(
	pcm_part_open_t &codec_open, HANDLE &&intro, HANDLE &&loop
)
{
	std::unique_ptr<pcm_part_t> intro_sgm;
	std::unique_ptr<pcm_part_t> loop_sgm;

	intro_sgm = codec_open(std::move(intro));
	if(!intro_sgm) {
		if(loop != INVALID_HANDLE_VALUE) {
			CloseHandle(loop);
		}
		return nullptr;
	}
	if(loop != INVALID_HANDLE_VALUE) {
		loop_sgm = codec_open(std::move(loop));
		if(!loop_sgm) {
			return nullptr;
		}
		if(intro_sgm->pcmf != loop_sgm->pcmf) {
			bgmmod_errorf(
				"PCM format mismatch between intro and loop parts!\n"
				"\xE2\x80\xA2 Intro: %s\n"
				"\xE2\x80\xA2 Loop: %s",
				intro_sgm->pcmf.to_string().str,
				loop_sgm->pcmf.to_string().str
			);
			return nullptr;
		}
	}
	return std::make_unique<track_pcm_t>(
		std::move(intro_sgm), std::move(loop_sgm)
	);
}
/// -----------

/// Codecs
/// ------
std::unique_ptr<pcm_part_t> mp3_open(HANDLE &&stream);
std::unique_ptr<pcm_part_t> vorbis_open(HANDLE &&stream);
std::unique_ptr<pcm_part_t> flac_open(HANDLE &&stream);

// Sorted from lowest to highest quality.
const codec_t CODECS[3] = {
	".mp3", mp3_open,
	".ogg", vorbis_open,
	".flac", flac_open,
};
#define RESOLVE_HINT "{flac > ogg > mp3}"
/// ------

/// Stack file resolution
/// ---------------------
std::unique_ptr<track_t> stack_bgm_resolve(const stringref_t &basename)
{
	log_printf(
		"(BGM) Resolving %.*s." RESOLVE_HINT "... ",
		basename.len, basename.str
	);

	const stringref_t game = json_object_get(runconfig_get(), "game");
	auto mod_fn_len =
		game.len + 1 + basename.len + LOOP_INFIX.len + LONGEST_CODEC_LEN + 1;

	VLA(char, mod_fn, mod_fn_len);
	defer({ VLA_FREE(mod_fn) });

	char *mod_fn_p = mod_fn;
	if(game.str) {
		mod_fn_p = stringref_copy_advance_dst(mod_fn_p, game);
		*(mod_fn_p++) = '/';
	}
	const auto *mod_fn_basename = mod_fn_p;
	mod_fn_p = stringref_copy_advance_dst(mod_fn_p, basename);

	auto chain = json_array();
	defer({ json_decref(chain); });

	// Patch root
	for(const auto &codec : CODECS) {
		stringref_copy_advance_dst(mod_fn_p, codec.ext);
		json_array_append_new(chain, json_string(mod_fn_basename));
	}
	// Game directory
	for(const auto &codec : CODECS) {
		stringref_copy_advance_dst(mod_fn_p, codec.ext);
		json_array_append_new(chain, json_string(mod_fn));
	}

	stack_chain_iterate_t sci = { 0 };
	while(stack_chain_iterate(&sci, chain, SCI_BACKWARDS, nullptr)) {
		auto intro = patch_file_stream(sci.patch_info, sci.fn);
		auto loop = INVALID_HANDLE_VALUE;
		if(intro != INVALID_HANDLE_VALUE) {
			patch_print_fn(sci.patch_info, sci.fn);

			const auto &codec = CODECS[sci.step % elementsof(CODECS)];
			// Kinda ugly, but makes sure that we keep using whatever
			// else we might have added to the link of the chain.
			auto sci_fn_end = sci.fn;
			size_t sci_basename_len = 0;
			while(sci_fn_end[0] != '\0') {
				if(sci_fn_end[0] == '.') {
					sci_basename_len = sci_fn_end - sci.fn;
				}
				sci_fn_end++;
			}

			mod_fn_p = memcpy_advance_dst(mod_fn, sci.fn, sci_basename_len);
			mod_fn_p = stringref_copy_advance_dst(mod_fn_p, LOOP_INFIX);
			stringref_copy_advance_dst(mod_fn_p, codec.ext);

			loop = patch_file_stream(sci.patch_info, mod_fn);
			if(loop != INVALID_HANDLE_VALUE) {
				patch_print_fn(sci.patch_info, mod_fn);
			}
			log_print("\n");

			return pcm_open(codec.open, std::move(intro), std::move(loop));
		}
	}
	log_print("not found\n");
	return nullptr;
}
/// ---------------------

/// Error reporting and debugging
/// -----------------------------
void bgmmod_verrorf(const char *text, va_list va)
{
	log_vmboxf("BGM modding error", MB_OK | MB_ICONERROR, text, va);
}

void bgmmod_errorf(const char *text, ...)
{
	va_list va;
	va_start(va, text);
	bgmmod_verrorf(text, va);
	va_end(va);
}
/// -----------------------------
