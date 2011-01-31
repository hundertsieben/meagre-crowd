/* Meagre-Crowd: A sparse distributed matrix solver testbench for performance benchmarking.
 * Copyright (C) 2010 Alistair Boyle <alistair.js.boyle@gmail.com>
 *
 *     This file is part of Meagre-Crowd.
 *
 *     Meagre-Crowd program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU Lesser General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU Lesser General Public License for more details.
 *
 *     You should have received a copy of the GNU Lesser General Public License
 *     along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "matrix.h"


struct enum2format_t {
  const enum matrix_format_t f;
  const char* s;
};

// how many matrix_format_t entries are there
#define matrix_format_t__MAX 6
static const struct enum2format_t enum2format[] = { {INVALID, "invalid"},
  {DROW,    "dense (rows)"},
  {DCOL,    "dense (columns)"},
  {SM_COO,  "COO"},
  {SM_CSC,  "CSC"},
  {SM_CSR,  "CSR"}
};

struct enum2base_t {
  const enum matrix_base_t b;
  const char* s;
};

const struct enum2base_t enum2base[] = { {FIRST_INDEX_ZERO, "zero"},
  {FIRST_INDEX_ONE, "one"}
};

void print_matrix( matrix_t* a );
void print_matrix( matrix_t* a ) {
  printf( "    %zux%zu (nz:%zu) %s%s%s%s %s\n",
          a->m, a->n, a->nz,
          enum2format[a->format].s,
          ( a->dd == NULL ) ? " !dd" : "",
          ( a->ii == NULL ) ? " !ii" : "",
          ( a->jj == NULL ) ? " !jj" : "",
          ( a->base == FIRST_INDEX_ZERO ) ? "base0" : "base1" );
  if ( a->format == SM_COO && a->data_type == REAL_DOUBLE ) {
    printf( "      dd:" );
    int i;
    for ( i = 0; i < a->nz; i++ )
      printf( " %e", ( double )(( double* )( a->dd ) )[i] );
    printf( "\n" );
    printf( "      ii:" );
    for ( i = 0; i < a->nz; i++ )
      printf( " %d", ( unsigned int )(( unsigned int* )( a->ii ) )[i] );
    printf( "\n" );
    printf( "      jj:" );
    for ( i = 0; i < a->nz; i++ )
      printf( " %d", ( unsigned int )(( unsigned int* )( a->jj ) )[i] );
    printf( "\n" );
  }
}


void test_formats( matrix_t* a );
void test_formats( matrix_t* a ) {
  matrix_t* b = copy_matrix( a );
  assert( b != NULL );
  print_matrix( b );

  int i, j, k, l, ret;
  for ( i = 0;i < 2;i++ ) { // from two types of base: 0 or 1
    for ( j = 0; j < matrix_format_t__MAX; j++ ) { // storage formats
      enum matrix_format_t old_format = b->format;
      printf( "%s -> %s (base %s -> %s)\n", enum2format[b->format].s, enum2format[j].s, enum2base[b->base].s, enum2base[i].s );
      ret = convert_matrix( b, ( enum matrix_format_t ) j, ( enum matrix_base_t ) i );
      if ( j == 0 ) { // INVALID
        assert( ret != 0 );
        assert( b->format == old_format );
        b->format = INVALID; // force it to be invalid so we can test the conversions
      }
      else {
        assert( ret == 0 );
        assert( b->format == ( enum matrix_format_t ) j );
        if (( b->format == DROW ) || ( b->format == DCOL ) )
          assert( b->base == FIRST_INDEX_ZERO );
        else
          assert( b->base == ( enum matrix_base_t ) i );
        assert( cmp_matrix( a, b ) == 0 );
      }

      for ( k = 0;k < 2;k++ ) { // to base
        for ( l = 1; l < matrix_format_t__MAX; l++ ) { // to all other storage formats
          matrix_t* c = copy_matrix( b );
          assert( c != NULL );

          printf( "  %s -> %s (base %s -> %s)\n", enum2format[j].s, enum2format[l].s, enum2base[i].s, enum2base[k].s );
          ret = convert_matrix( c, ( enum matrix_format_t ) l, ( enum matrix_base_t ) k );
          if (( j == 0 ) || ( l == 0 ) ) { // INVALID -> x OR x -> INVALID
            assert( ret != 0 );
            assert( c->format == ( enum matrix_format_t ) j );
          }
          else {
            print_matrix( b );
            print_matrix( c );
            assert( ret == 0 );
            assert( c->format == ( enum matrix_format_t ) l );
            if (( c->format == DROW ) || ( c->format == DCOL ) )
              assert( c->base == FIRST_INDEX_ZERO );
            else
              assert( c->base == ( enum matrix_base_t ) k );
            printf( "    cmp_matrix(b,c) = %d\n", cmp_matrix( b, c ) );
            printf( "    cmp_matrix(c,b) = %d\n", cmp_matrix( c, b ) );
            assert( cmp_matrix( b, c ) == 0 );
            assert( cmp_matrix( c, b ) == 0 );
          }

          free_matrix( c );
        }
      }
      // return to old format
      if ( b->format == INVALID )
        b->format = old_format;
    }
  }
  free_matrix( b );
}
// TODO check that if it starts as invalid, it stays invalid type
// TODO check that if it isn't invalid, it fails when asking to become invalid type

void test_basic();
void test_basic() {
  // try mucking around with an INVALID matrix
  matrix_t* a = malloc_matrix();
  assert( a != NULL );
  *a = ( matrix_t ) {
    0
  };
  assert( validate_matrix( a ) == 0 );

  matrix_t* b = copy_matrix( a );
  assert( b != NULL );
  assert( validate_matrix( b ) == 0 );
  assert( cmp_matrix( a, b ) == 0 );

  free_matrix( b );
  free_matrix( a );

  // create a couple of test matrices
  // TODO generate random matrices?
  matrix_t* c = malloc_matrix();
  assert( c != NULL );
  *c = ( matrix_t ) {
    0
  };
  c->m = 8;
  c->n = 9;
  c->nz = 4;
  c->base = FIRST_INDEX_ZERO;
  c->format = SM_COO;
  // TODO try other symmetries/locations
  c->data_type = REAL_DOUBLE; // TODO try other data types
  c->dd = malloc( sizeof( double ) * ( c->nz ) );
  c->ii = malloc( sizeof( unsigned int ) * ( c->nz ) );
  c->jj = malloc( sizeof( unsigned int ) * ( c->nz ) );
  assert( c->dd != NULL );
  assert( c->ii != NULL );
  assert( c->jj != NULL );
  {
    int i;
    double* d = c->dd;
    for ( i = 0;i < c->nz; i++ ) {
      d[i] = ( double ) i + 10.0;
      c->ii[i] = i * 2;
      c->jj[i] = i + 4;
    }
    assert( validate_matrix( c ) == 0 );
  }

  // try converting between all types
  test_formats( c );
  free_matrix( c );

  // TODO do some cmp_matrix's that are supposed to fail in different ways

  // TODO test behaviour of an emtpy matrix (nz=0) in all formats
}

int main( int argc, char **argv ) {
  test_basic();
  return 0;
}
