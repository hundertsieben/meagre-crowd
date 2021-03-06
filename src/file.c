/* Meagre-Crowd: A sparse distributed matrix solver testbench for performance benchmarking.
 * Copyright (C) 2010 Alistair Boyle <alistair.js.boyle@gmail.com>
 *
 *     This file is part of Meagre-Crowd.
 *
 *     Meagre-Crowd program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include "file.h"
#include <stdio.h> // fprintf, fopen, fclose, fgets
#include <string.h> // strnlen, strcmp
#include <ctype.h> // tolower
#include <assert.h>

#include "matrix.h"

#include <bebop/util/init.h>
#include <bebop/util/enumerations.h>
#include <bebop/smc/sparse_matrix.h>
#include <bebop/smc/sparse_matrix_ops.h> // load_sparse_matrix
#include <bebop/smc/coo_matrix.h> // convert to coo

#ifdef HAVE_MATIO
#include <matio.h>
#endif

// load a .mat matlab matrix
// will load the first matrix (sparse or dense) in the file
// returns 0 on success
static inline
int read_mat(char const * const filename, matrix_t * const A);

// save a .mat matlab matrix
// will store a single matrix (sparse or dense) in a .mat file
// returns 0 on success
static
int write_mat(char const * const filename, matrix_t * const A);

// Other formats we should support:
//   Harwell-Boeing (CSC format),
//   GAMFF (NASA Ames) (graphs, sparse matrices as HB?, dense matrices)

// input FILE f
// output object    is -1 unrecognized, 0 matrix
//        format    is -1 unrecognized, 0 array, 1 coordinate
//        datatype  is -1 unrecognized, 0 real, 1 integer, 2 complex, 3 pattern
//        symmetry  is -1 unrecognized, 0 general, 1 symmetric, 2 skew-symmetric, 3 hermitian
//        rows, cols, nz are dimensions of the matrix
//        comments are malloc-ed copy of the comments from the file
// returns  0 on success, <0 on failure (readmm_strerror(ret) for description)
//         -1 if EOF before parsing header,
//         -2 if not Matrix Market format
//         -3 if long lines are encountered (MatrixMarket specifies max length of 1024)
static inline int readmm_header(FILE* f, int* object, int* format, int* datatype, int* symmetry, int* rows, int* cols,
                                int* nz, char** comments);
static inline int readmm_data_dense(FILE* f, matrix_t* A, int datatype, int symmetry, int rows, int cols, int nz);
static inline int readmm_data_sparse(FILE* f, matrix_t* A, int datatype, int symmetry, int rows, int cols, int nz);

// read a MatrixMarket formatted file
// input  'filename' to read
//        'pref_dense_format'  - if the file turns out to be a dense matrix we'd prefer it to be stored in this format
//           valid values: DCOL, DROW
//        'pref_sparse_format' - if the file turns out to be a sparse matrix we'd prefer it to be stored in this format
//           valid values: SM_COO, SM_CSC, SM_CSR
// output '*A' is a pointer to the matrix structure to load the data into,
//                   if NULL no data is loaded
//        '**comments' is a pointer to a string that will be allocated, of all
//                   comments collected from the file (for use with
//                   structured comments, parsed by some other function,
//                   if NULL, no comments are returned
// returns  0 on success, <0 on failure
//          readmm_error(ret) gives a string explaining the error
int readmm(char const * const filename, matrix_t * const A, char** comments);

char const * readmm_strerror(int err);

char const * readmm_strerror(int err)
{
    char const * ret;
    switch (err) {
        case 0:
            ret = "success";
            break;
        case -1:
            ret = "memory allocation failure";
            break;
        case -2:
            ret = "can't open file";
            break;
        case -3:
            ret = "unexpected MatrixMarket header object (matrix)";
            break;
        case -4:
            ret = "unrecognized MatrixMarket header format (coordinate or array)";
            break;
        case -5:
            ret = "unrecognized MatrixMarket header data type";
            break;
        case -11:
            ret = "EOF before header";
            break;
        case -12:
            ret = "not MatrixMarket format, bad header";
            break;
        case -13:
            ret = "lines exceeding 1024 characters";
            break;
        default:
            ret = "unknown";
            break;
    }
    return ret;
}

int readmm(char const * const filename, matrix_t * const A, char** comments)
{
    // references:
    // [1] http://math.nist.gov/MatrixMarket/formats.html
    // [2] The Matrix Market Exchange Formats: Initial Design,
    //     R. F. Boisvert, R. Pozo, K. Remington,
    //     Applied and Computational Mathematics Division, NIST
    //     http://math.nist.gov/MatrixMarket/reports/MMformat.ps

    // MatrixMarket format notes:
    // sparse coordinate format: COO
    // dense format: column-oriented (DCOL)
    //   TODO to express a preference for DROW format, set that in the input matrix, 'A'
    // type: real, complex, integer, pattern
    // symmetry: general, symmetric, skew-symmetric, Hermitian
    //   for symmetric types only the *lower triangular* portion is stored
    //   for skew-symmetric matrices, diagonal is zero, so left out too

    // require "%%MatrixMarket " as first 15 characters
    // "%%MatrixMarket <object> <format> [qualifier ...]"
    // object could be vector, matrix, directed graph
    // format type is the storage format (coordinate, array)
    // qualifiers are fields, symmetry, etc
    // data is one entry per line
    // additional restrictions:
    //  * lines are limited 1024 characters
    //  * blanks lines can appear in any line after the first
    //  * data is separated by one or more "blanks" (spaces?)
    //  * real data is floating-point decimal, optionally use E-format exponential
    //  * all indices are 1-based
    //  * text is case-insensitive

    // expect sparse:
    // "%%MatrixMarket matrix coordinate <datatype> <sym>"
    // "%<comments>" -- zero or more lines
    // "  <rows> <columns> <non-zeros>"
    // "  <row> <col> <real>" -- indices are base-1, not base-0 REAL
    // "  <row> <col> <real> <imag>" -- indices are base-1, not base-0 COMPLEX

    // expect dense:
    // "%%MatrixMarket matrix array <type> <sym>"
    // "%<comments>" -- zero or more lines
    // "<rows> <columns>"
    // "<real>" -- REAL, column major order
    // "<real> <imag>" -- COMPLEX

    // additional restrictions: (kind of common-sense)
    //  * for coordinate AND array, "Hermitian" matrix types can only be "complex"
    //  * pattern matrices can only be coordinate format, and general or symmetric

    // <format> = coordinate, array
    // <datatype> = real, integer, complex, pattern
    // <sym> = general, symmetric, skew-symmetric, hermitian

    // Symmetry
    // general: no symmetry
    // symmetric A(i,j) = A(j,i)       (on or below diagonal) (square matrices)
    // skew-symmetric A(i,j) = -A(j,i)       (below diagonal) (square matrices)
    // hermitian      A(i,j) = A(j,i)* (on or below diagonal) (square matrices)

    // extensions:
    // * structured comments
    // * format specializations (pretty printing?)
    // * new object and format types
    //    <object> = graphs, grids, vectors?
    //    <format> = elemental (FEM), band, Toeplitz? (50% less storage or keen interest)

    // example structured comments
    //  "%%Harwell-Boeing collection"
    //  "%"
    //  "%HB FILE_NAME   abcd.mtx"
    //  "%HB KEY         MMEXMPL1"
    //  "%HB OBJECT      matrix"
    //  "%HB FORMAT      coordinate"
    //  "%HB QUALIFIRES  real general"
    //  "%HB DESCRIPTION Unsymmetric matrix from example"
    //  "%HB CONTRIBUTOR R. Boisvert (email@email.com)"
    //  "%HB DATE        June 24, 1996"
    //  "%HB PRECISION   4"
    //  "%HB DATA_LINES  9"
    //  "%"
    //  "% some more comments"

    // TODO could have verbose documentation about the format inside the file as comments

    FILE* f = fopen(filename, "r");
    if (f == NULL)
        return -2;

    int object, format, datatype, symmetry, rows, cols, nz;
    int ret;
    // NULL = ignoring comments
    ret = readmm_header(f, &object, &format, &datatype, &symmetry, &rows, &cols, &nz, NULL);
    if (ret < 0) {
        fclose(f);
        return ret - 10;
    }
    // output object    is -1 unrecognized, 0 matrix
    //        format    is -1 unrecognized, 0 array, 1 coordinate
    //        datatype  is -1 unrecognized, 0 real, 1 integer, 2 complex, 3 pattern
    //        symmetry  is -1 unrecognized, 0 general, 1 symmetric, 2 skew-symmetric, 3 hermitian
    //        rows, cols, nz are dimensions of the matrix
    //        comments are malloc-ed copy of the comments from the file

    // check we got the expected object type == matrix
    if (object != 0) {
        fclose(f);
        return -3;
    }

    clear_matrix(A);
    A->m = rows;
    A->n = cols;
    A->nz = nz;
    A->base = FIRST_INDEX_ONE;
    A->location = LOWER_TRIANGULAR;  // TODO test for this at the end and report error
    switch (format) {
        case 0:  // array
            ret = readmm_data_dense(f, A, datatype, symmetry, rows, cols, nz);
            break;
        case 1:  // COO
            ret = readmm_data_sparse(f, A, datatype, symmetry, rows, cols, nz);
            break;
        default:
            ret = -4;
    }

    fclose(f);
    return ret;
}

// consumes any leading spaces then reads the next avaiable int
// input string ptr
// outputs string ptr, first character after int
//         integer i
//         double k[2] = {real, imaginary}
//         integer n, number of values: n = 1 for real, n = 2 for complex
// returns 0 on success, -1 NaN, -2 not an int/double, -3 sscanf failure (conversion, errno has err code)
static inline int convert_k(char const* const s, double* k, int* n);
static inline int convert_ijk(char const * const s, int* i, int* j, double* k, int* n);
static inline int convert_int(char const ** string, int* i);
static inline int convert_float(char const ** string, double* i);

int readmm_data_dense(FILE* f, matrix_t* A, int datatype, int symmetry, int rows, int cols, int nz)
{
    A->format = DCOL;

    assert(symmetry == 0);  // TODO handle other symmetries, adjust nz
    A->sym = SM_UNSYMMETRIC;

    switch (datatype) {  // real, int, complex, pattern
        case 0:
            A->dd = malloc(nz * sizeof(double));
            A->data_type = SM_REAL;
            break;
        case 1:
            A->dd = malloc(nz * sizeof(int));
            assert(0);  // TODO can't handle int data
            break;
        case 2:
            A->dd = malloc(nz * sizeof(double) * 2);
            A->data_type = SM_COMPLEX;
            break;
        default:
            return -5;  // Note: can't have a dense format and pattern data type
    }
    if ((datatype != 3) && (A->dd == NULL)) {
        return -1;  // malloc failure
    }

    // read in the data
    double* d = A->dd;
    const int CHARS = 1025;
    char line[CHARS];
    char* lp;
    lp = "1";
    while (lp != NULL) {
        lp = fgets(line, CHARS, f);
    }

    // get the data
    int n;
    int ret = convert_k(lp, d, &n);
    if (ret != 0)
        return ret - 20;  // bad data
    if (((datatype == 0) && (n != 1)) || ((datatype == 2) && (n != 2)))
        return -3;  //bad data (unexpected complex/real)

    switch (datatype) {
        case 0:  // real
            d += 1;
            break;
        case 2:  // complex
            d += 2;
            break;
        default:
            assert(0);
            break;
    }
    return 0;
}

// TODO refactor and merge with readmm_data_dense?
int readmm_data_sparse(FILE* f, matrix_t* A, int datatype, int symmetry, int rows, int cols, int nz)
{
    A->format = SM_COO;

    switch (symmetry) {  // 0 general, 1 symmetric, 2 skew-symmetric, 3 hermitian
        case 0:
            A->sym = SM_UNSYMMETRIC;
            break;
        case 1:
            A->sym = SM_SYMMETRIC;
            break;
        case 2:
            A->sym = SM_SKEW_SYMMETRIC;
            break;
        case 3:
            A->sym = SM_HERMITIAN;
            break;
        default:
            assert(0);  // shouldn't be able to get here (checked before now)
    }

    // allocate memory
    A->ii = malloc(nz * sizeof(unsigned int));
    A->jj = malloc(nz * sizeof(unsigned int));
    switch (datatype) {  // real, int, complex, pattern
        case 0:
            A->dd = malloc(nz * sizeof(double));
            A->data_type = SM_REAL;
            break;
        case 1:
            A->dd = malloc(nz * sizeof(int));
            assert(0);  // TODO can't handle int data
            break;
        case 2:
            A->dd = malloc(nz * sizeof(double) * 2);
            A->data_type = SM_COMPLEX;
            break;
        default:
            return -5;  // Note: can't have a dense format and pattern data type
    }
    if ((A->ii == NULL) || (A->jj == NULL) || ((datatype != 3) && (A->dd == NULL)))
        return -1;  // malloc failure

    // read in the data
    // TODO assert that its safe to cast these based on rows/cols/nz
    int* ii = (int*) A->ii;
    int* jj = (int*) A->jj;
    double* d = A->dd;
    const int CHARS = 1025;  //can only read 1024 chars per line
    char line[CHARS];
    char* lp = NULL;
    int i;
    for (i = 0; i < nz; i++) {
        lp = fgets(line, CHARS, f);
        if (lp == NULL)  // fgets returns NULL if assumed line is already beyond EOF
            return -2;  // bad read

        // get the data
        int n;
        int ret = convert_ijk(lp, ii, jj, d, &n);
        if (ret != 0)
            return ret - 20;  // bad data
        if (((datatype == 0) && (n != 1)) || ((datatype == 2) && (n != 2)))
            return -3;  //bad data (unexpected complex/real)

        // advance the data pointers
        ii++;
        jj++;
        switch (datatype) {
            case 0:  // real
                d += 1;
                break;
            case 2:  // complex
                d += 2;
                break;
            default:
                assert(0);
                break;
        }
    }

    return 0;
}

static inline int is_eol(const char c);
static inline int readmm_header(FILE* f, int* object, int* format, int* datatype, int* symmetry, int* rows, int* cols,
                                int* nz, char** comments)
{
    assert(f != NULL);

    // valid field types
    const char* header[]     = { "%%matrixmarket", NULL };
    const char* objects[]    = { "matrix", NULL };
    const char* formats[]    = { "array", "coordinate", NULL };
    const char* datatypes[]  = { "real", "integer", "complex", "pattern", NULL };
    const char* symmetries[] = { "general", "symmetric", "skew-symmetric", "hermitian", NULL };

    const int FIELDS = 5;
    int dummy;
    char const ** const fields[] = { header, objects, formats, datatypes, symmetries, NULL };
    int * outputs[] = { &dummy, object, format, datatype, symmetry, NULL };

    const int CHARS = 1025;
    char line[CHARS];

    // read the first line of the file (stops at EOF or newline, null terminated)
    const char* lp = fgets(line, CHARS, f);
    if (lp == NULL)
        return -1;  // error or EOF w/ no characters

    // initialize results
    for (int i = 0; i < FIELDS; i++) {
        *(outputs[i]) = -1;
    }
    *rows = *cols = *nz = 0;
    if (comments != NULL) {
        free(*comments);
        *comments = NULL;
    }

    // loop through the strings to decode the header
    int ret = 0;
    char const **fp = fields[0];
    int fi = 0;
    lp = line;  // reset string ptr to start-of-line
    while ((fp != NULL) && (ret == 0)) {
        int sp = 0;
        while ((fp[sp] != NULL) && (sp >= 0)) {
            // compare the string
            const size_t chars_in_string = strlen(fp[sp]);
            if (strncasecmp(lp, fp[sp], chars_in_string) != 0) {
                sp++;
                continue;
            }

            // if it matches, check that the next character is ' ', '\n'
            char const * const lpt = lp + chars_in_string;  // temporary pointer for looking ahead
            if (!isspace(*lpt)) {
                sp++;
                continue;
            }

            // on match save result and set sp = -1 to break out of the loop
            lp += chars_in_string;
            *(outputs[fi]) = sp;
            sp = -1;
        }
        // see if we recognized this field, no need to go further if we're lost
        if (*outputs[fi] == -1)
            ret = -2;  // unrecognized format
        fi++;
        fp = fields[fi];

        // and consume any leading spaces before looking at the next string
        while (isblank(*lp))
            lp++;
    }
    // no need to go further if we're already confused
    if (ret != 0)
        return ret;

    // store comments if requested
    if (comments != NULL) {
        assert(0);  // TODO unfinished code
    }
    else {  // just get past the comments
        do {
            // check for long lines which would screw up this parsing
            // the first line ending aught to be in here somewhere...
            while (isprint(*lp))
                lp++;
            if (!is_eol(*lp))
                return -3;  // line too long

            // read in the next line
            lp = fgets(line, CHARS, f);
        }
        while ((*lp == '%') || is_eol(*lp));
    }
    // lp now points to the first non-blank line after the comments

    // find matrix dimensions
    if (*format == 0) {  // dense array, expecting "  <rows> <columns>"
        int err1 = convert_int(&lp, rows);
        int err2 = convert_int(&lp, cols);
        *nz = (*rows) * (*cols);

        if (err1 | err2)
            return -2;
    }
    else {  // must be COO format, expecting "  <rows> <columns> <non-zeros>"
        int err1 = convert_int(&lp, rows);
        int err2 = convert_int(&lp, cols);
        int err3 = convert_int(&lp, nz);

        if (err1 | err2 | err3)
            return -2;
    }

    // final check for end of line
    while (isblank(*lp))
        lp++;
    if (!is_eol(*lp))
        return -3;  // line too long

    return ret;
}

int convert_k(char const * const s, double* k, int* n)
{
    int ret;
    const char* sp = s;

    // get the first value
    *n = 1;
    if ((ret = convert_float(&sp, k)) != 0)
        return ret;

    // see if there's more data on this line
    while (isblank(*sp))
        sp++;
    if (is_eol(*sp))
        return 0;

    // must be another digit, assume we're dealing with complex data
    *n = 2;
    if ((ret = convert_float(&sp, k + 1)) != 0)
        return ret;

    // all done
    return 0;
}

int convert_ijk(char const * const s, int* i, int* j, double* k, int* n)
{
    int ret;
    const char* sp = s;

    if ((ret = convert_int(&sp, i)) != 0)
        return ret;

    if ((ret = convert_int(&sp, j)) != 0)
        return ret;

    return convert_k(sp, k, n);
}

// assumes null terminated line!
int convert_int(const char** s, int* i)
{
    // advance past any leading whitespace
    while (isblank(**s))
        *s += 1;

    // check the next character is a digit
    if (!isdigit(**s))
        return -1;  // not a number

    // check we then have only digits, followed by a space or line end
    // otherwise it might be a float
    char const * const sp = *s;  // save while we look-ahead
    while (isdigit(**s))
        *s += 1;
    if (!isspace(**s))
        return -2;  // not an int

    // convert assuming we have an integer
    const int n = sscanf(sp, "%d", i);
    if (n == 1)
        return 0;  // success
    else
        return -3;  // conversion failure (errno is set w/ info)
}

// assumes null terminated line!
int convert_float(const char** s, double* i)
{
    // advance past any leading whitespace
    while (isblank(**s))
        *s += 1;

    // check the next character is a digit
    if ((!isdigit(**s)) && (**s != '-') && (**s != '+'))
        return -1;  // not a number

    // check we then have only digits, followed by a space or line end
    // otherwise it might be a float
    char const * const sp = *s;  // save while we look-ahead
    while (isdigit(**s) || (**s == '.') || (**s == 'e') || (**s == 'E') || (**s == '-') || (**s == '+'))
        *s += 1;
    if (!isspace(**s))
        return -2;  // not an int

    // convert assuming we have an integer
    const int n = sscanf(sp, "%lg", i);
    if (n == 1)
        return 0;  // success
    else
        return -3;  // conversion failure (errno is set w/ info)
}

int is_eol(const char c)
{  // end of line excluding ' ' & \t
    return isspace(c) && !isblank(c);
}

static int _identify_format_from_extension(char* n, enum sparse_matrix_file_format_t* ext, int is_input);

// load a matrix from file "n" into matrix A
// returns 0: success, <0 failure
int load_matrix(char* n, matrix_t* A)
{
    assert(A != NULL);
    if (n == NULL) {
        fprintf( stderr, "input error: No input specified (-i)\n");
        return 1;  // failure
    }
    // make sure we don't have a memory leak
    clear_matrix(A);

    int ret;
    enum sparse_matrix_file_format_t ext;
    if ((ret = _identify_format_from_extension(n, &ext, 1)) != 0)
        return ret;

    switch (ext) {
        case MATRIX_MARKET:
            ret = readmm(n, A, NULL);
            break;  // NULL = ignore comments
        case MATLAB:
            ret = read_mat(n, A);
            break;
        case HARWELL_BOEING:
            ret = 100;
            assert(0);
            break;  // shouldn't be able to get here
        default:
            fprintf( stderr, "input error: format not recognized");
    }
    if (ret != 0) {
        fprintf( stderr, "input error: Failed to load matrix\n");  // TODO move these printouts to main...
        return ret;
    }

    if (A->sym == SM_UNSYMMETRIC)
        detect_matrix_symmetry(A);

    return 0;  // success
}

static inline
int writemm(char const* const filename, matrix_t* AA, char const * const comment, enum sparse_matrix_file_format_t ext);

// save a matrix into file "n" from matrix A
// returns 0: success, 1: failure
int save_matrix(matrix_t* AA, char* n)
{
    if (n == NULL) {
        fprintf( stderr, "output error: No output specified (-o)\n");
        return 1;  // failure
    }

    int ret;
    enum sparse_matrix_file_format_t ext;
    if ((ret = _identify_format_from_extension(n, &ext, 0)) != 0)
        return ret;

    switch (ext) {
        case MATRIX_MARKET:
            ret = writemm(n, AA, NULL, ext);
            break;  // NULL = ignore comments
        case MATLAB:
            ret = write_mat(n, AA);
            break;
        case HARWELL_BOEING:
            ret = 100;
            assert(0);
            break;  // shouldn't be able to get here
    }
    if (ret != 0)
        fprintf( stderr, "output error: Failed to store matrix\n");  // TODO move these printouts to main...
    return ret;
}

static inline
int writemm(char const* const filename, matrix_t* AA, char const * const comment, enum sparse_matrix_file_format_t ext)
{
    // make sure we're in the right format (COO) and base 0
    int ret;
    if ((ret = convert_matrix(AA, SM_COO, FIRST_INDEX_ZERO)) != 0)
        return ret;
    assert(AA->format == SM_COO);
    assert(AA->base == FIRST_INDEX_ZERO);

    assert(AA->sym == SM_UNSYMMETRIC);
    assert(AA->data_type == REAL_DOUBLE);

    // copy data into the BeBOP format
    struct sparse_matrix_t A;
    struct coo_matrix_t Acoo;
    A.format = COO;
    A.repr = &Acoo;
    Acoo.m = AA->m;
    Acoo.n = AA->n;
    Acoo.nnz = AA->nz;
    Acoo.II = (signed int*) AA->ii;
    Acoo.JJ = (signed int*) AA->jj;
    Acoo.val = AA->dd;
    Acoo.index_base = ZERO;
    Acoo.symmetry_type = UNSYMMETRIC;
    Acoo.value_type = REAL;
    Acoo.ownership = USER_DEALLOCATES;  // don't let BeBOP blow away our data

    save_sparse_matrix(filename, &A, ext);
    return 0;  // success
}

// returns number of rows in matrix A
inline unsigned int matrix_rows(const matrix_t* const A)
{
    if (A == NULL)
        return 0;
    return A->m;
}

// identify the file format from the extension
// return 0: success, 1: failure
// Note: static -- only visible w/in this file
static int _identify_format_from_extension(char* n, enum sparse_matrix_file_format_t* ext, int is_input)
{
    size_t s = strnlen(n, 100);
    char *e = n + s - 4;

    // strcmp returned match
    if ((s > 4) && (strncmp(e, ".mtx", 100) == 0)) {
        *ext = MATRIX_MARKET;
        return 0;  // success
    }
    else if ((s > 3) && (strncmp(e, ".hb", 100) == 0)) {
        *ext = HARWELL_BOEING;
        if (is_input)
            fprintf( stderr, "input error: Sorry Harwell-Boeing reader is broken\n");
        else
            fprintf( stderr, "output error: Sorry Harwell-Boeing writer is broken\n");
        return 1;  // failure
    }
    else if ((s > 3) && (strncmp(e, ".rb", 100) == 0)) {
        *ext = HARWELL_BOEING;
        if (is_input)
            fprintf( stderr, "input error: Sorry Rutherford-Boeing reader is broken\n");
        else
            fprintf( stderr, "output error: Sorry Rutherford-Boeing writer is broken\n");
        return 1;  // failure
    }
    else if ((s > 4) && (strncmp(e - 1, ".mat", 100) == 0)) {
        *ext = MATLAB;
#ifdef HAVE_MATIO
        return 0;
#else
        if ( is_input )
        fprintf( stderr, "input error: Matlab file reader was not enabled\n" );
        else
        fprintf( stderr, "output error: Matlab file writer was not enabled\n" );
        return 1;  // failure
#endif // HAVE_MATIO
        fprintf( stderr, "error: Sorry Matlab writer is broken\n");
    }  // TODO test if the matlab reader is actually busted
    else {
        if (is_input)
            fprintf( stderr, "input error: Unrecognized file extension\n");
        else
            fprintf( stderr, "output error: Unrecognized file extension\n");
        return 1;  // failure
    }
}

// save a .mat matlab matrix
// will store a single matrix (sparse or dense) in a .mat file
// returns 0 on success
static
int write_mat(char const * const filename, matrix_t * const A)
{
#ifndef HAVE_MATIO
    return 1;
#else
    const char created_by[] = "created by " PACKAGE_STRING;  // "created by Meagre-Crowd x.y.z"
    mat_t* matfp;
    matfp = Mat_Create(filename, created_by);
    if (matfp == NULL)
        return 1;  // failed to open file

    // create a matrix convert into
    matvar_t* t = NULL;
    int ret = 0;
    if (A->format == DCOL || A->format == DROW) {  // dense
        ret = convert_matrix(A, DCOL, FIRST_INDEX_ZERO);  // DROW -> DCOL if DROW
        if (ret != 0)
            ret = 2;  // conversion failure

        if (ret == 0) {
            // TODO check for integer overflow in cast from unsigned int -> int
            size_t dims[] = { A->m, A->n };
            t = Mat_VarCreate("x", MAT_C_DOUBLE, MAT_T_DOUBLE, 2,  // always at least rank 2
                              dims, A->dd, 0  // MAT_F_COMPLEX if complex, could avoid copying data: MAT_F_DONT_COPY_DATA if not sparse
                              );

            if (t == NULL) {
                ret = 3;  // failed to malloc data for storage
            }
            else {
                ret = Mat_VarWrite(matfp, t, 1);  // compress
                if (ret != 0)
                    ret = 4;  // failed data write
            }
        }
    }
    else {  // sparse
        assert(0);  // do we ever get sparse results?
    }

    Mat_Close(matfp);
    Mat_VarFree(t);
    return ret;  // success?
#endif
}

// load a .mat matlab matrix
// will load the first matrix (sparse or dense) in the file
// returns 0 on success
static
int read_mat(char const * const filename, matrix_t * const A)
{
#ifndef HAVE_MATIO
    return 1;
#else
    const int LOCAL_DEBUG = 0;
    mat_t* matfp;
    matfp = Mat_Open(filename, MAT_ACC_RDONLY);
    if (matfp == NULL)
        return 1;  // failed to open file

    matvar_t* t;
    int more_data = 1;

    while (more_data) {
        t = Mat_VarReadNextInfo(matfp);
        if (t == NULL) {
            return 2;  // no suitable variable found
        }
// TODO decide if this is the one:    if(t->
        if (1) {
            more_data = 0;
        }
        else {  // keep going
            Mat_VarFree(t);
            t = NULL;
        }
    }

    {  // load the selected variable, including data
        Mat_Rewind(matfp);
        matvar_t* tt = Mat_VarRead(matfp, t->name);
        Mat_VarFree(t);
        t = tt;
        if (LOCAL_DEBUG)
            Mat_VarPrint(tt, 1);
    }

    // debug info
    if (LOCAL_DEBUG && t->name != NULL)
        printf("%s: loaded variable %s\n", filename, t->name);

    // checks and data handling
    int ret = 0;

    if (t->rank > 2 || t->rank <= 0) {  // number of dimensions
        ret = 2;
    }
    else if (t->data_type != MAT_T_DOUBLE) {
        if (LOCAL_DEBUG)
            printf("data_type=%d\n", t->data_type);
        ret = 3;
    }
    else if (t->isComplex) {
        ret = 10;  // TODO can't handle complex matrices yet
    }
    else if (t->isLogical) {
        ret = 11;  // TODO can't handle logicals yet
    }
    else {
        if (t->rank == 1) {
            A->m = t->dims[0];  // rows
            A->n = 1;  // cols
        }
        else {
            A->m = t->dims[0];  // rows
            A->n = t->dims[1];  // cols
        }
        A->sym = SM_UNSYMMETRIC;
        //if(t->data_type == MAT_T_DOUBLE) {
        A->data_type = REAL_DOUBLE;
        //} // TODO complex, single precision, various sized integers

        if (t->class_type == MAT_C_SPARSE) {  // t.data = sparse_t in CSC format
            // Note that Matlab will save('-v4'...) a sparse matrix
            // to version 4 format without complaint but it appears
            // to be gibberish as far as MatIO is concerned
            mat_sparse_t* st = t->data;
            A->nz = st->ndata;  //st->nzmax has the actual size of the allocated st->data
            A->format = SM_CSC;
            // transfer the data pointer into our strucut
            // TODO check for negative values in ir/jc before throwing away their signs
            A->ii = (unsigned int*) st->ir;
            st->ir = NULL;
            A->jj = (unsigned int*) st->jc;
            st->jc = NULL;
            A->dd = st->data;
            st->data = NULL;
        }
        else if (t->class_type == MAT_C_DOUBLE) {
            A->nz = A->m * A->n;
            A->format = DCOL;
            // transfer the data pointer into our struct
            A->dd = t->data;
            t->data = NULL;
        }
        else {
            ret = 4;  // unknown class of data structure
        }
    }

    // DEBUG
    if (LOCAL_DEBUG && validate_matrix(A) != 0) {
        ret = 50;
        fprintf(stderr, "problem loading .mat matrix file\n");
        printf_matrix("  ", A);
    }

    // sanity checks
    // t.dims[] is don't-care
    // t.isGlobal is don't-care

    Mat_Close(matfp);
    Mat_VarFree(t);
    return ret;  // success?
#endif
}

