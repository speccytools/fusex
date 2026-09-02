/* expression.c: A numeric expression
   Copyright (c) 2003-2017 Philip Kendall
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugger_internals.h"
#include "fuse.h"
#include "mempool.h"
#include "ui/ui.h"
#include "utils.h"

typedef enum expression_type {

  DEBUGGER_EXPRESSION_TYPE_INTEGER,
  DEBUGGER_EXPRESSION_TYPE_UNARYOP,
  DEBUGGER_EXPRESSION_TYPE_BINARYOP,
  DEBUGGER_EXPRESSION_TYPE_SYSVAR,
  DEBUGGER_EXPRESSION_TYPE_VARIABLE,

} expression_type;

enum precedence_t {

  /* Lowest precedence */
  PRECEDENCE_LOGICAL_OR,
  PRECEDENCE_LOGICAL_AND,
  PRECEDENCE_BITWISE_OR,
  PRECEDENCE_BITWISE_XOR,
  PRECEDENCE_BITWISE_AND,
  PRECEDENCE_EQUALITY,
  PRECEDENCE_COMPARISON,
  PRECEDENCE_SHIFT,
  PRECEDENCE_ADDITION,
  PRECEDENCE_MULTIPLICATION,
  PRECEDENCE_NEGATE,
  PRECEDENCE_DEREFERENCE,

  PRECEDENCE_ATOMIC,
  /* Highest precedence */

};

struct unaryop_type {

  int operation;
  debugger_expression *op;

};

struct binaryop_type {

  int operation;
  debugger_expression *op1, *op2;

};

struct debugger_expression {

  expression_type type;
  enum precedence_t precedence;

  union {
    int integer;
    struct unaryop_type unaryop;
    struct binaryop_type binaryop;
    char *variable;
    int system_variable;
  } types;

};

static libspectrum_dword evaluate_unaryop( const struct unaryop_type *unaryop );
static libspectrum_dword evaluate_binaryop( const struct binaryop_type *binary );

static int deparse_unaryop( char *buffer, size_t length,
			    const struct unaryop_type *unaryop );
static int deparse_binaryop( char *buffer, size_t length,
			     const struct binaryop_type *binaryop );
static int
brackets_necessary( int top_operation, debugger_expression *operand );
static int is_non_associative( int operation );

static enum precedence_t
unaryop_precedence( int operation )
{
  switch( operation ) {

  case '!': case '~': case '-': return PRECEDENCE_NEGATE;

  case DEBUGGER_TOKEN_DEREFERENCE:
    return PRECEDENCE_DEREFERENCE;

  default:
    ui_error( UI_ERROR_ERROR, "unknown unary operator %d", operation );
    fuse_abort();
  }
}

static enum precedence_t
binaryop_precedence( int operation )
{
  switch( operation ) {

  case DEBUGGER_TOKEN_LOGICAL_OR: return PRECEDENCE_LOGICAL_OR;
  case DEBUGGER_TOKEN_LOGICAL_AND: return PRECEDENCE_LOGICAL_AND;
  case    '|':		    return PRECEDENCE_BITWISE_OR;
  case    '^':		    return PRECEDENCE_BITWISE_XOR;
  case    '&':		    return PRECEDENCE_BITWISE_AND;
  case    '+': case    '-': return PRECEDENCE_ADDITION;
  case    '*': case    '/': case    '%': return PRECEDENCE_MULTIPLICATION;

  case DEBUGGER_TOKEN_LEFT_SHIFT:
  case DEBUGGER_TOKEN_RIGHT_SHIFT:
    return PRECEDENCE_SHIFT;

  case DEBUGGER_TOKEN_EQUAL_TO:
  case DEBUGGER_TOKEN_NOT_EQUAL_TO:
    return PRECEDENCE_EQUALITY;

  case    '<': case    '>':
  case DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO:
  case DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO:
    return PRECEDENCE_COMPARISON;

  default:
    ui_error( UI_ERROR_ERROR, "unknown binary operator %d", operation );
    fuse_abort();
  }
}

debugger_expression*
debugger_expression_new_number( libspectrum_dword number, int pool )
{
  debugger_expression *exp;

  exp = mempool_new( pool, debugger_expression, 1 );

  exp->type = DEBUGGER_EXPRESSION_TYPE_INTEGER;
  exp->precedence = PRECEDENCE_ATOMIC;
  exp->types.integer = number;

  return exp;
}

debugger_expression*
debugger_expression_new_binaryop( int operation, debugger_expression *operand1,
				  debugger_expression *operand2, int pool )
{
  debugger_expression *exp;

  exp = mempool_new( pool, debugger_expression, 1 );

  exp->type = DEBUGGER_EXPRESSION_TYPE_BINARYOP;
  exp->precedence = binaryop_precedence( operation );

  exp->types.binaryop.operation = operation;
  exp->types.binaryop.op1 = operand1;
  exp->types.binaryop.op2 = operand2;

  return exp;
}

debugger_expression*
debugger_expression_new_unaryop( int operation, debugger_expression *operand,
				 int pool )
{
  debugger_expression *exp;

  exp = mempool_new( pool, debugger_expression, 1 );

  exp->type = DEBUGGER_EXPRESSION_TYPE_UNARYOP;
  exp->precedence = unaryop_precedence( operation );

  exp->types.unaryop.operation = operation;
  exp->types.unaryop.op = operand;

  return exp;
}

debugger_expression*
debugger_expression_new_system_variable( const char *type, const char *detail,
                                         int pool )
{
  debugger_expression *exp;
  int system_variable;

  system_variable = debugger_system_variable_find( type, detail );
  if( system_variable == -1 ) {
    ui_error( UI_ERROR_WARNING, "System variable %s:%s not known", type,
              detail );
    return NULL;
  }

  exp = mempool_new( pool, debugger_expression, 1 );

  exp->type = DEBUGGER_EXPRESSION_TYPE_SYSVAR;
  exp->precedence = PRECEDENCE_ATOMIC;
  exp->types.system_variable = system_variable;

  return exp;
}

debugger_expression*
debugger_expression_new_variable( const char *name, int pool )
{
  debugger_expression *exp;

  exp = mempool_new( pool, debugger_expression, 1 );

  exp->type = DEBUGGER_EXPRESSION_TYPE_VARIABLE;
  exp->precedence = PRECEDENCE_ATOMIC;
  exp->types.variable = mempool_strdup( pool, name );

  return exp;
}

void
debugger_expression_delete( debugger_expression *exp )
{
  switch( exp->type ) {
    
  case DEBUGGER_EXPRESSION_TYPE_INTEGER:
  case DEBUGGER_EXPRESSION_TYPE_SYSVAR:
    break;

  case DEBUGGER_EXPRESSION_TYPE_UNARYOP:
    debugger_expression_delete( exp->types.unaryop.op );
    break;

  case DEBUGGER_EXPRESSION_TYPE_BINARYOP:
    debugger_expression_delete( exp->types.binaryop.op1 );
    debugger_expression_delete( exp->types.binaryop.op2 );
    break;

  case DEBUGGER_EXPRESSION_TYPE_VARIABLE:
    libspectrum_free( exp->types.variable );
    break;
  }
    
  libspectrum_free( exp );
}

debugger_expression*
debugger_expression_copy( const debugger_expression *src )
{
  debugger_expression *dest;

  dest = libspectrum_new( debugger_expression, 1 );
  if( !dest ) return NULL;

  dest->type = src->type;
  dest->precedence = src->precedence;

  switch( dest->type ) {

  case DEBUGGER_EXPRESSION_TYPE_INTEGER:
    dest->types.integer = src->types.integer;
    break;

  case DEBUGGER_EXPRESSION_TYPE_UNARYOP:
    dest->types.unaryop.operation = src->types.unaryop.operation;
    dest->types.unaryop.op = debugger_expression_copy( src->types.unaryop.op );
    if( !dest->types.unaryop.op ) {
      libspectrum_free( dest );
      return NULL;
    }
    break;

  case DEBUGGER_EXPRESSION_TYPE_BINARYOP:
    dest->types.binaryop.operation = src->types.binaryop.operation;
    dest->types.binaryop.op1 =
      debugger_expression_copy( src->types.binaryop.op1 );
    if( !dest->types.binaryop.op1 ) {
      libspectrum_free( dest );
      return NULL;
    }
    dest->types.binaryop.op2 =
      debugger_expression_copy( src->types.binaryop.op2 );
    if( !dest->types.binaryop.op2 ) {
      debugger_expression_delete( dest->types.binaryop.op1 );
      libspectrum_free( dest );
      return NULL;
    }
    break;

  case DEBUGGER_EXPRESSION_TYPE_SYSVAR:
    dest->types.system_variable = src->types.system_variable;
    break;

  case DEBUGGER_EXPRESSION_TYPE_VARIABLE:
    dest->types.variable = utils_safe_strdup( src->types.variable );
    break;
  }

  return dest;
}

libspectrum_dword
debugger_expression_evaluate( const debugger_expression *exp )
{
  switch( exp->type ) {

  case DEBUGGER_EXPRESSION_TYPE_INTEGER:
    return exp->types.integer;

  case DEBUGGER_EXPRESSION_TYPE_UNARYOP:
    return evaluate_unaryop( &( exp->types.unaryop ) );

  case DEBUGGER_EXPRESSION_TYPE_BINARYOP:
    return evaluate_binaryop( &( exp->types.binaryop ) );

  case DEBUGGER_EXPRESSION_TYPE_SYSVAR:
    return debugger_system_variable_get( exp->types.system_variable );

  case DEBUGGER_EXPRESSION_TYPE_VARIABLE:
    return debugger_variable_get( exp->types.variable );

  }

  ui_error( UI_ERROR_ERROR, "unknown expression type %d", exp->type );
  fuse_abort();
}

static libspectrum_dword
evaluate_unaryop( const struct unaryop_type *unary )
{
  switch( unary->operation ) {

  case '!': return !debugger_expression_evaluate( unary->op );
  case '~': return ~debugger_expression_evaluate( unary->op );
  case '-': return -debugger_expression_evaluate( unary->op );

  case DEBUGGER_TOKEN_DEREFERENCE:
    return readbyte_internal( debugger_expression_evaluate( unary->op ) );

  }

  ui_error( UI_ERROR_ERROR, "unknown unary operator %d", unary->operation );
  fuse_abort();
}

static libspectrum_dword
evaluate_binaryop( const struct binaryop_type *binary )
{
  switch( binary->operation ) {

  case '+': return debugger_expression_evaluate( binary->op1 ) +
		   debugger_expression_evaluate( binary->op2 );

  case '-': return debugger_expression_evaluate( binary->op1 ) -
		   debugger_expression_evaluate( binary->op2 );

  case '*': return debugger_expression_evaluate( binary->op1 ) *
		   debugger_expression_evaluate( binary->op2 );

  case '/': {
      libspectrum_dword op2 = debugger_expression_evaluate( binary->op2 );
      if( op2 == 0 ) {
        ui_error( UI_ERROR_ERROR, "divide by 0" );
        return 0;
      }
      return debugger_expression_evaluate( binary->op1 ) / op2;
    }

  case '%': {
      libspectrum_dword op2 = debugger_expression_evaluate( binary->op2 );
      if( op2 == 0 ) {
        ui_error( UI_ERROR_ERROR, "modulo by 0" );
        return 0;
      }
      return debugger_expression_evaluate( binary->op1 ) % op2;
    }

  case DEBUGGER_TOKEN_LEFT_SHIFT: {
      libspectrum_dword count = debugger_expression_evaluate( binary->op2 );
      if( count >= 32 ) return 0;
      return debugger_expression_evaluate( binary->op1 ) << count;
    }

  case DEBUGGER_TOKEN_RIGHT_SHIFT: {
      libspectrum_dword count = debugger_expression_evaluate( binary->op2 );
      if( count >= 32 ) return 0;
      return debugger_expression_evaluate( binary->op1 ) >> count;
    }

  case DEBUGGER_TOKEN_EQUAL_TO:
            return debugger_expression_evaluate( binary->op1 ) ==
                   debugger_expression_evaluate( binary->op2 );

  case DEBUGGER_TOKEN_NOT_EQUAL_TO:
	    return debugger_expression_evaluate( binary->op1 ) !=
		   debugger_expression_evaluate( binary->op2 );

  case '>': return debugger_expression_evaluate( binary->op1 ) >
		   debugger_expression_evaluate( binary->op2 );

  case '<': return debugger_expression_evaluate( binary->op1 ) <
	           debugger_expression_evaluate( binary->op2 );

  case DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO:
	    return debugger_expression_evaluate( binary->op1 ) <=
		   debugger_expression_evaluate( binary->op2 );

  case DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO:
	    return debugger_expression_evaluate( binary->op1 ) >=
		   debugger_expression_evaluate( binary->op2 );

  case '&': return debugger_expression_evaluate( binary->op1 ) &
	           debugger_expression_evaluate( binary->op2 );

  case '^': return debugger_expression_evaluate( binary->op1 ) ^
		   debugger_expression_evaluate( binary->op2 );

  case '|': return debugger_expression_evaluate( binary->op1 ) |
		   debugger_expression_evaluate( binary->op2 );

  case DEBUGGER_TOKEN_LOGICAL_AND:
	    return debugger_expression_evaluate( binary->op1 ) &&
		   debugger_expression_evaluate( binary->op2 );

  case DEBUGGER_TOKEN_LOGICAL_OR:
	    return debugger_expression_evaluate( binary->op1 ) ||
		   debugger_expression_evaluate( binary->op2 );

  }

  ui_error( UI_ERROR_ERROR, "unknown binary operator %d", binary->operation );
  fuse_abort();
}

int
debugger_expression_deparse( char *buffer, size_t length,
			     const debugger_expression *exp )
{
  switch( exp->type ) {

  case DEBUGGER_EXPRESSION_TYPE_INTEGER:
    if( debugger_output_base == 10 ) {
      snprintf( buffer, length, "%u", exp->types.integer );
    } else {
      snprintf( buffer, length, "0x%x", exp->types.integer );
    }
    return 0;

  case DEBUGGER_EXPRESSION_TYPE_UNARYOP:
    return deparse_unaryop( buffer, length, &( exp->types.unaryop ) );

  case DEBUGGER_EXPRESSION_TYPE_BINARYOP:
    return deparse_binaryop( buffer, length, &( exp->types.binaryop ) );

  case DEBUGGER_EXPRESSION_TYPE_SYSVAR:
    debugger_system_variable_text( buffer, length, exp->types.system_variable );
    return 0;

  case DEBUGGER_EXPRESSION_TYPE_VARIABLE:
    snprintf( buffer, length, "$%s", exp->types.variable );
    return 0;

  }

  ui_error( UI_ERROR_ERROR, "unknown expression type %d", exp->type );
  fuse_abort();
}
  
static int
deparse_unaryop( char *buffer, size_t length,
		 const struct unaryop_type *unaryop )
{
  char *operand_buffer; const char *operation_string = NULL;
  const char *operation_suffix = "";
  int brackets_possible = 1;
  int brackets = 0;

  int error;

  operand_buffer = libspectrum_new( char, length );

  error = debugger_expression_deparse( operand_buffer, length, unaryop->op );
  if( error ) { libspectrum_free( operand_buffer ); return error; }

  switch( unaryop->operation ) {
  case '!': operation_string = "!"; break;
  case '~': operation_string = "~"; break;
  case '-': operation_string = "-"; break;
  case DEBUGGER_TOKEN_DEREFERENCE:
    operation_string = "[";
    operation_suffix = "]";
    brackets_possible = 0;
    break;

  default:
    ui_error( UI_ERROR_ERROR, "unknown unary operation %d",
	      unaryop->operation );
    fuse_abort();
  }

  if( brackets_possible )
    brackets = ( unaryop->op->precedence                  < 
                 unaryop_precedence( unaryop->operation )   );
    
  snprintf( buffer, length, "%s%s%s%s%s", operation_string,
	    brackets ? "( " : "", operand_buffer,
	    brackets ? " )" : "", operation_suffix );

  libspectrum_free( operand_buffer );

  return 0;
}

static int
deparse_binaryop( char *buffer, size_t length,
		  const struct binaryop_type *binaryop )
{
  char *operand1_buffer, *operand2_buffer; const char *operation_string = NULL;
  int brackets_necessary1, brackets_necessary2;

  int error;

  operand1_buffer = libspectrum_new( char, 2 * length );
  operand2_buffer = &operand1_buffer[ length ];

  error = debugger_expression_deparse( operand1_buffer, length,
				       binaryop->op1 );
  if( error ) { libspectrum_free( operand1_buffer ); return error; }

  error = debugger_expression_deparse( operand2_buffer, length,
				       binaryop->op2 );
  if( error ) { libspectrum_free( operand1_buffer ); return error; }

  switch( binaryop->operation ) {
  case    '+': operation_string = "+";  break;
  case    '-': operation_string = "-";  break;
  case    '*': operation_string = "*";  break;
  case    '/': operation_string = "/";  break;
  case    '%': operation_string = "%";  break;
  case DEBUGGER_TOKEN_LEFT_SHIFT: operation_string = "<<"; break;
  case DEBUGGER_TOKEN_RIGHT_SHIFT: operation_string = ">>"; break;
  case DEBUGGER_TOKEN_EQUAL_TO: operation_string = "=="; break;
  case DEBUGGER_TOKEN_NOT_EQUAL_TO: operation_string = "!="; break;
  case    '<': operation_string = "<";  break;
  case    '>': operation_string = ">";  break;
  case DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO: operation_string = "<="; break;
  case DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO: operation_string = ">="; break;
  case    '&': operation_string = "&";  break;
  case    '^': operation_string = "^";  break;
  case    '|': operation_string = "|";  break;
  case DEBUGGER_TOKEN_LOGICAL_AND: operation_string = "&&"; break;
  case DEBUGGER_TOKEN_LOGICAL_OR: operation_string = "||"; break;

  default:
    ui_error( UI_ERROR_ERROR, "unknown binary operation %d",
	      binaryop->operation );
    fuse_abort();
  }

  brackets_necessary1 = brackets_necessary( binaryop->operation,
					    binaryop->op1 );
  brackets_necessary2 = brackets_necessary( binaryop->operation,
					    binaryop->op2 );

  snprintf( buffer, length, "%s%s%s %s %s%s%s",
	    brackets_necessary1 ? "( " : "", operand1_buffer,
	    brackets_necessary1 ? " )" : "",
	    operation_string,
	    brackets_necessary2 ? "( " : "", operand2_buffer,
	    brackets_necessary2 ? " )" : "" );

  libspectrum_free( operand1_buffer );

  return 0;
}

/* When deparsing, do we need to put brackets around `operand' when
   being used as an operand of the binary operation `top_operation'? */
static int
brackets_necessary( int top_operation, debugger_expression *operand )
{
  enum precedence_t top_precedence, bottom_precedence;
  
  top_precedence = binaryop_precedence( top_operation );
  bottom_precedence = operand->precedence;

  /* If the top level operation has a higher precedence than the
     bottom level operation, we always need brackets */
  if( top_precedence > bottom_precedence ) return 1;

  /* If the two operations are of equal precedence, we need brackets
     i) if the top level operation is non-associative, or
     ii) if the operand is a non-associative operation

     Note the assumption here that all things with a precedence equal to
     a binary operator are also binary operators

     Strictly, we don't need brackets in either of these cases, but
     otherwise the user is going to have to remember operator
     left-right associativity; I think things are clearer with
     brackets in.
  */
  if( top_precedence == bottom_precedence ) {

    if( is_non_associative( top_operation ) ) return 1;

    /* Sanity check */
    if( operand->type != DEBUGGER_EXPRESSION_TYPE_BINARYOP ) {
      ui_error( UI_ERROR_ERROR,
		"binary operator has same precedence as non-binary operator" );
      fuse_abort();
    }

    return is_non_associative( operand->types.binaryop.operation );
  }

  /* Otherwise (ie if the top level operation is of lower precedence
     than the bottom, or both operators have equal precedence and
     everything is associative) we don't need brackets */
  return 0;
}

/* Is a binary operator non-associative? */
static int
is_non_associative( int operation )
{
  switch( operation ) {

  /* Simple cases */
  case '+': case '*': return 0;
  case '-': case '/': case '%': return 1;

  /* Shift operators are non-associative */
  case DEBUGGER_TOKEN_LEFT_SHIFT: return 1;
  case DEBUGGER_TOKEN_RIGHT_SHIFT: return 1;

  /* None of the comparison operators are associative due to them
     returning truth values */
  case DEBUGGER_TOKEN_EQUAL_TO:
  case DEBUGGER_TOKEN_NOT_EQUAL_TO:
  case '<': case '>':
  case DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO:
  case DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO:
    return 1;

  /* The logical operators are associative */
  case DEBUGGER_TOKEN_LOGICAL_AND: return 0;
  case DEBUGGER_TOKEN_LOGICAL_OR: return 0;

  /* The bitwise operators are also associative (consider them as
     vectorised logical operators) */
  case    '&': return 0;
  case    '^': return 0;
  case    '|': return 0;

  }

  /* Should never get here */
  ui_error( UI_ERROR_ERROR, "unknown binary operation %d", operation );
  fuse_abort();
}

/* Unit tests for the debugger expression evaluator */

static int
eval_integer_test( libspectrum_dword value, libspectrum_dword expected )
{
  debugger_expression *expr;
  libspectrum_dword result;

  expr = debugger_expression_new_number( value, MEMPOOL_UNTRACKED );
  result = debugger_expression_evaluate( expr );
  debugger_expression_delete( expr );

  if( result != expected ) {
    printf( "expression eval 'integer': expected %u, got %u\n",
            (unsigned)expected, (unsigned)result );
    return 1;
  }

  return 0;
}

static int
eval_variable_test( const char *name, libspectrum_dword expected,
                    const char *label )
{
  debugger_expression *expr;
  libspectrum_dword result;

  expr = debugger_expression_new_variable( name, MEMPOOL_UNTRACKED );
  result = debugger_expression_evaluate( expr );
  debugger_expression_delete( expr );

  if( result != expected ) {
    printf( "expression eval '%s': expected %u, got %u\n",
            label, (unsigned)expected, (unsigned)result );
    return 1;
  }

  return 0;
}

static int
eval_binary_test( int op, libspectrum_dword v1, libspectrum_dword v2,
                  libspectrum_dword expected, const char *label )
{
  debugger_expression *left, *right, *expr;
  libspectrum_dword result;

  left  = debugger_expression_new_number( v1, MEMPOOL_UNTRACKED );
  right = debugger_expression_new_number( v2, MEMPOOL_UNTRACKED );
  expr  = debugger_expression_new_binaryop( op, left, right, MEMPOOL_UNTRACKED );
  result = debugger_expression_evaluate( expr );
  debugger_expression_delete( expr );

  if( result != expected ) {
    printf( "expression eval '%s': expected %u, got %u\n",
            label, (unsigned)expected, (unsigned)result );
    return 1;
  }

  return 0;
}

static int
eval_unary_test( int op, libspectrum_dword v, libspectrum_dword expected,
                 const char *label )
{
  debugger_expression *operand, *expr;
  libspectrum_dword result;

  operand = debugger_expression_new_number( v, MEMPOOL_UNTRACKED );
  expr    = debugger_expression_new_unaryop( op, operand, MEMPOOL_UNTRACKED );
  result  = debugger_expression_evaluate( expr );
  debugger_expression_delete( expr );

  if( result != expected ) {
    printf( "expression eval '%s': expected %u, got %u\n",
            label, (unsigned)expected, (unsigned)result );
    return 1;
  }

  return 0;
}

/* Deparse helper: takes ownership of expr and frees it after the check */
static int
deparse_test( debugger_expression *expr, const char *expected,
              const char *label )
{
  char buf[64];

  debugger_expression_deparse( buf, sizeof( buf ), expr );
  debugger_expression_delete( expr );

  if( strcmp( buf, expected ) ) {
    printf( "expression deparse '%s': expected '%s', got '%s'\n",
            label, expected, buf );
    return 1;
  }

  return 0;
}

/* Copy-and-eval helper: takes ownership of expr, copies it, deletes
   the original, evaluates the copy, and checks the result. */
static int
copy_eval_test( debugger_expression *expr, libspectrum_dword expected,
                const char *label )
{
  debugger_expression *copy;
  libspectrum_dword result;

  copy = debugger_expression_copy( expr );
  debugger_expression_delete( expr );
  result = debugger_expression_evaluate( copy );
  debugger_expression_delete( copy );

  if( result != expected ) {
    printf( "expression copy-eval '%s': expected %u, got %u\n",
            label, (unsigned)expected, (unsigned)result );
    return 1;
  }

  return 0;
}

/* Copy-and-deparse helper: takes ownership of expr, copies it, deletes
   the original, deparses the copy, and checks the result. */
static int
copy_deparse_test( debugger_expression *expr, const char *expected,
                   const char *label )
{
  debugger_expression *copy;
  char buf[64];

  copy = debugger_expression_copy( expr );
  debugger_expression_delete( expr );
  debugger_expression_deparse( buf, sizeof( buf ), copy );
  debugger_expression_delete( copy );

  if( strcmp( buf, expected ) ) {
    printf( "expression copy-deparse '%s': expected '%s', got '%s'\n",
            label, expected, buf );
    return 1;
  }

  return 0;
}

static libspectrum_dword
get_sysvar_sentinel( void )
{
  return 42;
}

int
debugger_expression_unittest( void )
{
  int r = 0;
  int saved_base;

  /* Integer literals */
  r += eval_integer_test( 0,  0 );
  r += eval_integer_test( 42, 42 );

  /* Arithmetic */
  r += eval_binary_test( '+',  3, 4,  7, "add" );
  r += eval_binary_test( '-', 10, 3,  7, "sub" );
  r += eval_binary_test( '*',  3, 4, 12, "mul" );
  r += eval_binary_test( '/', 12, 4,  3, "div" );

/* Only run on null UI as it creates a popup */
#ifdef UI_NULL
  /* Division by zero returns 0 without crashing */
  r += eval_binary_test( '/',  5, 0,  0, "div-by-zero" );
#endif

  /* Modulo operator */
  r += eval_binary_test( '%', 10, 3,  1, "mod" );
  r += eval_binary_test( '%',  9, 3,  0, "mod-exact" );

/* Only run on null UI as it creates a popup */
#ifdef UI_NULL
  /* Modulo by zero returns 0 without crashing */
  r += eval_binary_test( '%', 10, 0,  0, "mod-by-zero" );
#endif

  /* Bit-shift operators */
  r += eval_binary_test( DEBUGGER_TOKEN_LEFT_SHIFT,   1, 3,   8, "lshift" );
  r += eval_binary_test( DEBUGGER_TOKEN_LEFT_SHIFT,   1, 7, 128, "lshift-7" );
  r += eval_binary_test( DEBUGGER_TOKEN_LEFT_SHIFT,   1, 32,  0, "lshift-large" );
  r += eval_binary_test( DEBUGGER_TOKEN_RIGHT_SHIFT, 16, 2,   4, "rshift" );
  r += eval_binary_test( DEBUGGER_TOKEN_RIGHT_SHIFT,  8, 3,   1, "rshift-3" );
  r += eval_binary_test( DEBUGGER_TOKEN_RIGHT_SHIFT,  1, 32,  0, "rshift-large" );

  /* Bitwise operators */
  r += eval_binary_test( '&', 0xF0, 0xFF, 0xF0, "bitwise-and" );
  r += eval_binary_test( '|', 0x0F, 0xF0, 0xFF, "bitwise-or" );
  r += eval_binary_test( '^', 0xFF, 0x0F, 0xF0, "bitwise-xor" );

  /* Comparison operators */
  r += eval_binary_test( DEBUGGER_TOKEN_EQUAL_TO,               5, 5, 1, "eq-true" );
  r += eval_binary_test( DEBUGGER_TOKEN_EQUAL_TO,               5, 6, 0, "eq-false" );
  r += eval_binary_test( DEBUGGER_TOKEN_NOT_EQUAL_TO,           5, 6, 1, "ne-true" );
  r += eval_binary_test( DEBUGGER_TOKEN_NOT_EQUAL_TO,           5, 5, 0, "ne-false" );
  r += eval_binary_test( '<', 3, 5, 1, "lt-true" );
  r += eval_binary_test( '<', 5, 3, 0, "lt-false" );
  r += eval_binary_test( '>', 5, 3, 1, "gt-true" );
  r += eval_binary_test( '>', 3, 5, 0, "gt-false" );
  r += eval_binary_test( DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO,    5, 5, 1, "lte-eq" );
  r += eval_binary_test( DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO,    4, 5, 1, "lte-lt" );
  r += eval_binary_test( DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO,    6, 5, 0, "lte-false" );
  r += eval_binary_test( DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO, 5, 5, 1, "gte-eq" );
  r += eval_binary_test( DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO, 6, 5, 1, "gte-gt" );
  r += eval_binary_test( DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO, 4, 5, 0, "gte-false" );

  /* Logical operators */
  r += eval_binary_test( DEBUGGER_TOKEN_LOGICAL_AND, 1, 1, 1, "logical-and-true" );
  r += eval_binary_test( DEBUGGER_TOKEN_LOGICAL_AND, 1, 0, 0, "logical-and-false" );
  r += eval_binary_test( DEBUGGER_TOKEN_LOGICAL_OR,  1, 0, 1, "logical-or-true" );
  r += eval_binary_test( DEBUGGER_TOKEN_LOGICAL_OR,  0, 0, 0, "logical-or-false" );

  /* Unary operators */
  r += eval_unary_test( '!', 0,          1, "logical-not-zero" );
  r += eval_unary_test( '!', 1,          0, "logical-not-one" );
  r += eval_unary_test( '~', 0, 0xFFFFFFFF, "bitwise-not-zero" );
  r += eval_unary_test( '~', 0xFFFFFFFF,  0, "bitwise-not-ones" );

  /* Unary negation: unsigned arithmetic wraps modulo 2^32 */
  r += eval_unary_test( '-',          0,          0, "negate-zero" );
  r += eval_unary_test( '-',          1, 0xFFFFFFFF, "negate-one" );
  r += eval_unary_test( '-', 0xFFFFFFFF,          1, "negate-maxuint" );
  r += eval_unary_test( '-', 0x80000000, 0x80000000, "negate-midpoint" );

  /* Deparse tests: save and restore output base */
  saved_base = debugger_output_base;
  debugger_output_base = 16;

  r += deparse_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 + 0x4", "deparse-add" );

  r += deparse_test(
    debugger_expression_new_binaryop( '%',
      debugger_expression_new_number( 10, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number(  3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xa % 0x3", "deparse-mod" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LEFT_SHIFT,
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x1 << 0x3", "deparse-lshift" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_RIGHT_SHIFT,
      debugger_expression_new_number( 16, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number(  2, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x10 >> 0x2", "deparse-rshift" );

  /* is_non_associative('%') fix: deparsing (3 * 4) % 5 must bracket the
     left operand since % is non-associative and equal-precedence to *.
     Before the fix the missing '%' case in is_non_associative() would abort. */
  r += deparse_test(
    debugger_expression_new_binaryop( '%',
      debugger_expression_new_binaryop( '*',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "( 0x3 * 0x4 ) % 0x5", "deparse-mod-non-assoc" );

  /* Plain arithmetic operators without deparse coverage yet */
  r += deparse_test(
    debugger_expression_new_binaryop( '-',
      debugger_expression_new_number( 7, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x7 - 0x3", "deparse-sub" );

  r += deparse_test(
    debugger_expression_new_binaryop( '*',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 * 0x4", "deparse-mul" );

  r += deparse_test(
    debugger_expression_new_binaryop( '/',
      debugger_expression_new_number( 12, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number(  4, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xc / 0x4", "deparse-div" );

  /* Comparison operators */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_EQUAL_TO,
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 6, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x5 == 0x6", "deparse-eq" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_NOT_EQUAL_TO,
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 6, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x5 != 0x6", "deparse-ne" );

  r += deparse_test(
    debugger_expression_new_binaryop( '<',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < 0x5", "deparse-lt" );

  r += deparse_test(
    debugger_expression_new_binaryop( '>',
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x5 > 0x3", "deparse-gt" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LESS_THAN_OR_EQUAL_TO,
      debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x4 <= 0x5", "deparse-lte" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_GREATER_THAN_OR_EQUAL_TO,
      debugger_expression_new_number( 6, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x6 >= 0x5", "deparse-gte" );

  /* Comparison non-associativity: at equal precedence the result is always
     bracketed (regardless of position) because comparison operators return
     truth values and chaining them is almost always a mistake. */

  /* 3 == (4 == 5): right operand at equal EQUALITY precedence — brackets */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_EQUAL_TO,
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_EQUAL_TO,
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 == ( 0x4 == 0x5 )", "deparse-eq-non-assoc" );

  /* 3 < (4 < 5): right operand at equal COMPARISON precedence — brackets */
  r += deparse_test(
    debugger_expression_new_binaryop( '<',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '<',
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < ( 0x4 < 0x5 )", "deparse-lt-non-assoc-right" );

  /* (3 < 4) < 5: left operand at equal COMPARISON precedence — brackets */
  r += deparse_test(
    debugger_expression_new_binaryop( '<',
      debugger_expression_new_binaryop( '<',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "( 0x3 < 0x4 ) < 0x5", "deparse-lt-non-assoc-left" );

  /* (3 < 4) == 5: COMPARISON child of EQUALITY parent — higher prec, no
     brackets needed; re-parsing preserves the original evaluation order */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_EQUAL_TO,
      debugger_expression_new_binaryop( '<',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < 0x4 == 0x5", "deparse-lt-inside-eq" );

  /* 3 < (4 == 5): EQUALITY child of COMPARISON parent — lower prec forces
     brackets so re-parsing does not swap to (3 < 4) == 5 */
  r += deparse_test(
    debugger_expression_new_binaryop( '<',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_EQUAL_TO,
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < ( 0x4 == 0x5 )", "deparse-eq-inside-lt" );

  /* Bitwise operators */
  r += deparse_test(
    debugger_expression_new_binaryop( '&',
      debugger_expression_new_number( 0xF0, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 0xFF, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xf0 & 0xff", "deparse-bitwise-and" );

  r += deparse_test(
    debugger_expression_new_binaryop( '^',
      debugger_expression_new_number( 0xFF, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 0x0F, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xff ^ 0xf", "deparse-bitwise-xor" );

  r += deparse_test(
    debugger_expression_new_binaryop( '|',
      debugger_expression_new_number( 0x0F, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 0xF0, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xf | 0xf0", "deparse-bitwise-or" );

  /* Logical operators */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_AND,
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x1 && 0x1", "deparse-logical-and" );

  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_OR,
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 0, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x1 || 0x0", "deparse-logical-or" );

  /* Precedence interactions involving logical operators */

  /* && has higher precedence than ||: '||' parent with '&&' right child
     needs no brackets because && binds more tightly */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_OR,
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_AND,
        debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x1 || 0x1 && 0x0", "deparse-and-inside-or" );

  /* '&&' parent with '||' left child: || has lower precedence than &&
     so the left operand must be bracketed */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_AND,
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_OR,
        debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "( 0x1 || 0x0 ) && 0x1", "deparse-or-inside-and" );

  /* Comparison operators have higher precedence than &&: 'a < b && c > d'
     needs no brackets around either comparison */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_AND,
      debugger_expression_new_binaryop( '<',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '>',
        debugger_expression_new_number( 6, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < 0x5 && 0x6 > 0x4", "deparse-cmp-inside-and" );

  /* '&&' inside a comparison: && has lower precedence than '<' so the
     right operand must be bracketed */
  r += deparse_test(
    debugger_expression_new_binaryop( '<',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_LOGICAL_AND,
        debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 < ( 0x1 && 0x1 )", "deparse-and-inside-cmp" );

  /* '*' parent with '%' child: % is non-associative so even though both
     have the same precedence, the child must be bracketed */
  r += deparse_test(
    debugger_expression_new_binaryop( '*',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '%',
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 * ( 0x4 % 0x5 )", "deparse-mul-mod-non-assoc-right" );

  /* Binary '-' is non-associative: 3-(4-2) must bracket the right operand */
  r += deparse_test(
    debugger_expression_new_binaryop( '-',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '-',
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 2, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 - ( 0x4 - 0x2 )", "deparse-sub-non-assoc" );

  /* '/' is non-associative: the right operand '/' at equal precedence always
     needs brackets so 12/(4/2) is not confused with (12/4)/2 */
  r += deparse_test(
    debugger_expression_new_binaryop( '/',
      debugger_expression_new_number( 12, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '/',
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 2, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xc / ( 0x4 / 0x2 )", "deparse-div-non-assoc" );

  /* '<<' is non-associative: 1 << (2 << 3) brackets the right operand */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_LEFT_SHIFT,
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_LEFT_SHIFT,
        debugger_expression_new_number( 2, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x1 << ( 0x2 << 0x3 )", "deparse-lshift-non-assoc" );

  /* '>>' is non-associative: 16 >> (4 >> 2) brackets the right operand */
  r += deparse_test(
    debugger_expression_new_binaryop( DEBUGGER_TOKEN_RIGHT_SHIFT,
      debugger_expression_new_number( 16, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( DEBUGGER_TOKEN_RIGHT_SHIFT,
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 2, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x10 >> ( 0x4 >> 0x2 )", "deparse-rshift-non-assoc" );

  /* Mixed associativity at equal precedence: '+' is associative, '-' is
     non-associative, both have the same precedence.

     a + (b - c): the right operand is '-' (non-associative), so it must be
     bracketed even though the parent '+' is associative.  This exercises the
     is_non_associative(bottom_op) branch of brackets_necessary(). */
  r += deparse_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_number( 0xa, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '-',
        debugger_expression_new_number( 0xb, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0xc, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0xa + ( 0xb - 0xc )", "deparse-sub-inside-add-right" );

  /* (a + b) - c: the parent '-' is non-associative, so the left operand
     '+' must be bracketed even though '+' itself is associative.  This
     exercises the is_non_associative(top_op) branch of
     brackets_necessary(). */
  r += deparse_test(
    debugger_expression_new_binaryop( '-',
      debugger_expression_new_binaryop( '+',
        debugger_expression_new_number( 0xa, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0xb, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 0xc, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "( 0xa + 0xb ) - 0xc", "deparse-add-inside-sub-left" );

  /* Cross-precedence: lower-precedence child is bracketed inside a
     higher-precedence parent (add inside mul) */
  r += deparse_test(
    debugger_expression_new_binaryop( '*',
      debugger_expression_new_binaryop( '+',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "( 0x3 + 0x4 ) * 0x5", "deparse-add-inside-mul" );

  /* Higher-precedence child of lower-precedence parent needs no brackets */
  r += deparse_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      debugger_expression_new_binaryop( '*',
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x3 + 0x4 * 0x5", "deparse-mul-inside-add" );

  /* Unary negation deparse: plain integer and expression requiring brackets */
  r += deparse_test(
    debugger_expression_new_unaryop( '-',
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "-0x5", "deparse-negate" );

  r += deparse_test(
    debugger_expression_new_unaryop( '-',
      debugger_expression_new_binaryop( '+',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "-( 0x3 + 0x4 )", "deparse-negate-expr" );

  /* Unary '!' deparse: plain integer (no brackets) and expression (brackets
     needed because '+' has lower precedence than '!') */
  r += deparse_test(
    debugger_expression_new_unaryop( '!',
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "!0x5", "deparse-logical-not" );

  r += deparse_test(
    debugger_expression_new_unaryop( '!',
      debugger_expression_new_binaryop( '+',
        debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 4, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "!( 0x3 + 0x4 )", "deparse-logical-not-expr" );

  /* Unary '~' deparse: plain integer (no brackets) and expression (brackets
     needed because '|' has lower precedence than '~') */
  r += deparse_test(
    debugger_expression_new_unaryop( '~',
      debugger_expression_new_number( 15, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "~0xf", "deparse-bitwise-not" );

  r += deparse_test(
    debugger_expression_new_unaryop( '~',
      debugger_expression_new_binaryop( '|',
        debugger_expression_new_number( 0xF0, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0x0F, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "~( 0xf0 | 0xf )", "deparse-bitwise-not-expr" );

  r += deparse_test(
    debugger_expression_new_unaryop( DEBUGGER_TOKEN_DEREFERENCE,
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "[0x5]", "deparse-dereference" );

  r += deparse_test(
    debugger_expression_new_unaryop( DEBUGGER_TOKEN_DEREFERENCE,
      debugger_expression_new_binaryop( '+',
        debugger_expression_new_number( 0x100, MEMPOOL_UNTRACKED ),
        debugger_expression_new_number( 0x0f, MEMPOOL_UNTRACKED ),
        MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "[0x100 + 0xf]", "deparse-dereference-expr" );

  /* User-defined variable: deparse emits the name prefixed with '$' */
  r += deparse_test(
    debugger_expression_new_variable( "counter", MEMPOOL_UNTRACKED ),
    "$counter", "deparse-variable" );

  /* Variable inside a binary expression */
  r += deparse_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_variable( "counter", MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 1, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "$counter + 0x1", "deparse-variable-in-binop" );

  /* Variable evaluation: an unset variable returns 0 */
  r += eval_variable_test( "zz_undefined", 0, "variable-undefined-returns-zero" );

  /* Variable evaluation: set then get */
  debugger_variable_set( "zz_counter", 42 );
  r += eval_variable_test( "zz_counter", 42, "variable-get-after-set" );

  /* Variable inside an arithmetic expression: $zz_x * 2 == 20 */
  debugger_variable_set( "zz_x", 10 );
  {
    debugger_expression *var, *two, *prod;
    libspectrum_dword result;

    var  = debugger_expression_new_variable( "zz_x", MEMPOOL_UNTRACKED );
    two  = debugger_expression_new_number( 2, MEMPOOL_UNTRACKED );
    prod = debugger_expression_new_binaryop( '*', var, two, MEMPOOL_UNTRACKED );
    result = debugger_expression_evaluate( prod );
    debugger_expression_delete( prod );
    if( result != 20 ) {
      printf( "expression eval 'variable-in-binop-eval': expected 20, got %u\n",
              (unsigned)result );
      r++;
    }
  }

  /* debugger_expression_copy() tests: verify each expression type is deep-
     copied correctly.  The original is deleted before the copy is used so
     a shallow copy would produce garbage or a use-after-free error. */

  /* INTEGER copy: eval and deparse */
  r += copy_eval_test(
    debugger_expression_new_number( 99, MEMPOOL_UNTRACKED ),
    99, "copy-eval-integer" );

  r += copy_deparse_test(
    debugger_expression_new_number( 7, MEMPOOL_UNTRACKED ),
    "0x7", "copy-deparse-integer" );

  /* UNARYOP copy: negate 5 → 0xFFFFFFFB */
  r += copy_eval_test(
    debugger_expression_new_unaryop( '-',
      debugger_expression_new_number( 5, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    (libspectrum_dword)-5, "copy-eval-unaryop" );

  r += copy_deparse_test(
    debugger_expression_new_unaryop( '-',
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "-0x3", "copy-deparse-unaryop" );

  /* BINARYOP copy: 6 + 7 = 13 */
  r += copy_eval_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_number( 6, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 7, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    13, "copy-eval-binaryop" );

  r += copy_deparse_test(
    debugger_expression_new_binaryop( '+',
      debugger_expression_new_number( 2, MEMPOOL_UNTRACKED ),
      debugger_expression_new_number( 3, MEMPOOL_UNTRACKED ),
      MEMPOOL_UNTRACKED ),
    "0x2 + 0x3", "copy-deparse-binaryop" );

  /* VARIABLE copy: zz_counter was set to 42 above */
  r += copy_eval_test(
    debugger_expression_new_variable( "zz_counter", MEMPOOL_UNTRACKED ),
    42, "copy-eval-variable" );

  r += copy_deparse_test(
    debugger_expression_new_variable( "zz_counter", MEMPOOL_UNTRACKED ),
    "$zz_counter", "copy-deparse-variable" );

  /* SYSVAR: register a read-only test sentinel that returns 42 */
  debugger_system_variable_register( "test", "sentinel", get_sysvar_sentinel,
                                     NULL );

  r += deparse_test(
    debugger_expression_new_system_variable( "test", "sentinel",
                                             MEMPOOL_UNTRACKED ),
    "test:sentinel", "deparse-sysvar" );

  /* System variable evaluation */
  {
    debugger_expression *expr;
    libspectrum_dword result;

    expr = debugger_expression_new_system_variable( "test", "sentinel",
                                                    MEMPOOL_UNTRACKED );
    result = debugger_expression_evaluate( expr );
    debugger_expression_delete( expr );
    if( result != 42 ) {
      printf( "expression eval 'sysvar-eval': expected 42, got %u\n",
              (unsigned)result );
      r++;
    }
  }

  /* SYSVAR copy: original deleted before copy is used */
  r += copy_eval_test(
    debugger_expression_new_system_variable( "test", "sentinel",
                                             MEMPOOL_UNTRACKED ),
    42, "copy-eval-sysvar" );

  r += copy_deparse_test(
    debugger_expression_new_system_variable( "test", "sentinel",
                                             MEMPOOL_UNTRACKED ),
    "test:sentinel", "copy-deparse-sysvar" );

  /* Deparse in decimal base: small value and a value > INT_MAX */
  debugger_output_base = 10;

  r += deparse_test(
    debugger_expression_new_number( 42, MEMPOOL_UNTRACKED ),
    "42", "deparse-decimal-small" );

  /* 0x80000000 is 2147483648 as uint32_t but -2147483648 as int32_t.
     Using %d (signed) would produce the wrong negative result; %u is correct. */
  r += deparse_test(
    debugger_expression_new_number( 0x80000000, MEMPOOL_UNTRACKED ),
    "2147483648", "deparse-decimal-large" );

  debugger_output_base = saved_base;

  return r;
}

