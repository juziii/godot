/**************************************************************************/
/*  test_resource_importer_streamed_texture.cpp                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_resource_importer_streamed_texture)

#include "modules/modules_enabled.gen.h"

#if defined(TOOLS_ENABLED) && defined(MODULE_TINYEXR_ENABLED)
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "editor/import/resource_importer_streamed_texture.h"
#include "tests/test_utils.h"

namespace TestResourceImporterStreamedTexture {

static HashMap<StringName, Variant> import_options(const Ref<ResourceImporterStreamedTexture> &p_importer) {
	List<ResourceImporter::ImportOption> options;
	p_importer->get_import_options("", &options);
	HashMap<StringName, Variant> result;
	for (const ResourceImporter::ImportOption &option : options) {
		result[option.option.name] = option.default_value;
	}
	// An uncompressed HDR fixture exercises the real importer without requiring a GPU or codec.
	result["compress/hdr_compression"] = 0;
	result["compress/normal_map"] = 2;
	result["roughness/mode"] = 1;
	return result;
}

static Ref<Image> import_image(const Ref<ResourceImporterStreamedTexture> &p_importer, const Ref<Image> &p_image, const HashMap<StringName, Variant> &p_options) {
	const String source = TestUtils::get_temp_path("streamed_texture_import.exr");
	const String output = TestUtils::get_temp_path("streamed_texture_import");
	REQUIRE(p_image->save_exr(source) == OK);
	const String source_hash = FileAccess::get_sha256(source);
	List<String> variants;
	REQUIRE(p_importer->import(ResourceUID::INVALID_ID, source, output, p_options, &variants) == OK);
	CHECK(FileAccess::get_sha256(source) == source_hash);
	REQUIRE(variants.is_empty());

	Ref<FileAccess> file = FileAccess::open(output + ".stex", FileAccess::READ);
	REQUIRE(file.is_valid());
	CHECK(file->get_32() == 0x46545347); // GSTF.
	CHECK(file->get_32() == 1);
	const int width = file->get_32();
	const int height = file->get_32();
	const int data_size = file->get_32();
	const Image::Format format = Image::Format(file->get_32());
	const int mipmaps = file->get_32();
	file->seek(file->get_position() + (mipmaps + 1) * 8 + 16);
	Vector<uint8_t> data;
	data.resize(data_size);
	REQUIRE(file->get_buffer(data.ptrw(), data_size) == uint64_t(data_size));
	return memnew(Image(width, height, mipmaps > 0, format, data));
}

TEST_CASE("[StreamedTextureImporter] Size limit precedes mip generation and preserves the source") {
	Ref<ResourceImporterStreamedTexture> importer = memnew(ResourceImporterStreamedTexture);
	HashMap<StringName, Variant> options = import_options(importer);
	CHECK(int(options["process/size_limit"]) == 1024);
	CHECK(bool(options["process/fix_alpha_border"]));
	CHECK_FALSE(bool(options["process/premult_alpha"]));
	CHECK_FALSE(bool(options["process/normal_map_invert_y"]));

	struct SizeCase {
		int width;
		int height;
		int limit;
		int expected_width;
		int expected_height;
		int expected_mipmaps;
	};
	const SizeCase cases[] = {
		{ 2048, 512, -1, 1024, 256, 10 }, // Old import metadata without the option.
		{ 512, 2048, 1024, 256, 1024, 10 },
		{ 2048, 512, 256, 256, 64, 8 }, // Preserve an explicit smaller limit.
		{ 128, 64, 1024, 128, 64, 7 }, // Never upscale.
		{ 2048, 512, 0, 2048, 512, 11 }, // Explicit zero retains ordinary importer semantics.
		{ 4096, 1, 1024, 1024, 1, 10 }, // A very thin image must keep a nonzero dimension.
	};
	for (const SizeCase &test : cases) {
		CAPTURE(test.width);
		CAPTURE(test.height);
		CAPTURE(test.limit);
		if (test.limit < 0) {
			options.erase("process/size_limit");
		} else {
			options["process/size_limit"] = test.limit;
		}
		Ref<Image> source = memnew(Image(test.width, test.height, false, Image::FORMAT_RGBAF));
		source->fill(Color(0.25, 0.5, 0.75, 1.0));
		Ref<Image> imported = import_image(importer, source, options);
		REQUIRE(imported.is_valid());
		CHECK(imported->get_width() == test.expected_width);
		CHECK(imported->get_height() == test.expected_height);
		CHECK(imported->get_mipmap_count() == test.expected_mipmaps);
		CHECK(imported->get_pixel(0, 0).is_equal_approx(Color(0.25, 0.5, 0.75, 1.0)));
	}
}

TEST_CASE("[StreamedTextureImporter] Normal Y inversion affects the generated mip chain") {
	Ref<ResourceImporterStreamedTexture> importer = memnew(ResourceImporterStreamedTexture);
	HashMap<StringName, Variant> options = import_options(importer);
	options["process/normal_map_invert_y"] = true;
	Ref<Image> source = memnew(Image(8, 8, false, Image::FORMAT_RGBAF));
	source->fill(Color(0.5, 0.25, 0.75, 1.0));
	Ref<Image> imported = import_image(importer, source, options);
	CHECK(imported->get_pixel(0, 0).is_equal_approx(Color(0.5, 0.75, 0.75, 1.0)));
	imported->shrink_x2();
	CHECK(imported->get_pixel(0, 0).is_equal_approx(Color(0.5, 0.75, 0.75, 1.0)));
	CHECK(source->get_pixel(0, 0).is_equal_approx(Color(0.5, 0.25, 0.75, 1.0)));
}

} // namespace TestResourceImporterStreamedTexture
#endif // TOOLS_ENABLED && MODULE_TINYEXR_ENABLED
