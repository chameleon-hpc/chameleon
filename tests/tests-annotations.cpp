#include "catch2/catch.hpp"

#include "chameleon.h"
#include "../src/chameleon_common.h"

TEST_CASE( "set and get annotations", "[single-file]" ) {
    chameleon_annotations_t ann;
    chameleon_set_annotation_int(&ann, "Int", 42);
    chameleon_set_annotation_int64(&ann, "Int64", 21);
    chameleon_set_annotation_double(&ann, "Dbl", 3.14);
    chameleon_set_annotation_float(&ann, "Float", 5.87);
    chameleon_set_annotation_string(&ann, "Str", "Test123");

    int found = 0;
    int val_int;
    found = chameleon_get_annotation_int(&ann, "Int", &val_int);
    REQUIRE(found == 1);
    REQUIRE(val_int == 42);

    int64_t val_int64;
    found = chameleon_get_annotation_int64(&ann, "Int64", &val_int64);
    REQUIRE(found == 1);
    REQUIRE(val_int64 == 21);

    double val_dbl;
    found = chameleon_get_annotation_double(&ann, "Dbl", &val_dbl);
    REQUIRE(found == 1);
    REQUIRE(val_dbl == 3.14);

    float val_float;
    found = chameleon_get_annotation_float(&ann, "Float", &val_float);
    REQUIRE(found == 1);
    REQUIRE(val_float == 5.87f);

    char* val_string;
    found = chameleon_get_annotation_string(&ann, "Str", &val_string);
    REQUIRE(found == 1);
    REQUIRE(strcmp(val_string, "Test123") == 0);
}

TEST_CASE( "(de)serialization of annotations", "[single-file]" ) {
    chameleon_annotations_t ann;
    chameleon_set_annotation_int(&ann, "Int", 42);
    chameleon_set_annotation_int64(&ann, "Int64", 21);
    chameleon_set_annotation_double(&ann, "Dbl", 3.14);
    chameleon_set_annotation_float(&ann, "Float", 5.87);
    chameleon_set_annotation_string(&ann, "Str", "Test123");

    int32_t buff_size;
    void* buffer = ann.pack(&buff_size);

    chameleon_annotations_t ann2;
    ann2.unpack(buffer);

    // clean up again
    free(buffer);

    int found = 0;
    int val_int;
    found = chameleon_get_annotation_int(&ann2, "Int", &val_int);
    REQUIRE(found == 1);
    REQUIRE(val_int == 42);

    int64_t val_int64;
    found = chameleon_get_annotation_int64(&ann2, "Int64", &val_int64);
    REQUIRE(found == 1);
    REQUIRE(val_int64 == 21);

    double val_dbl;
    found = chameleon_get_annotation_double(&ann2, "Dbl", &val_dbl);
    REQUIRE(found == 1);
    REQUIRE(val_dbl == 3.14);

    float val_float;
    found = chameleon_get_annotation_float(&ann2, "Float", &val_float);
    REQUIRE(found == 1);
    REQUIRE(val_float == 5.87f);

    char* val_string;
    found = chameleon_get_annotation_string(&ann2, "Str", &val_string);
    REQUIRE(found == 1);
    REQUIRE(strcmp(val_string, "Test123") == 0);
}
