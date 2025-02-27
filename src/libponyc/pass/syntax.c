#include "syntax.h"
#include "../ast/id.h"
#include "../ast/parser.h"
#include "../ast/stringtab.h"
#include "../ast/token.h"
#include "../pkg/package.h"
#include "../pkg/platformfuns.h"
#include "../type/assemble.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"
#include <string.h>
#include <ctype.h>


#define DEF_ACTOR 0
#define DEF_CLASS 1
#define DEF_STRUCT 2
#define DEF_PRIMITIVE 3
#define DEF_TRAIT 4
#define DEF_INTERFACE 5
#define DEF_TYPEALIAS 6
#define DEF_ENTITY_COUNT 7

#define DEF_FUN (DEF_ENTITY_COUNT * 0)
#define DEF_BE (DEF_ENTITY_COUNT * 1)
#define DEF_NEW (DEF_ENTITY_COUNT * 2)
#define DEF_METHOD_COUNT (DEF_ENTITY_COUNT * 3)


typedef struct permission_def_t
{
  const char* desc;
  const char* permissions;
} permission_def_t;

// Element permissions are specified by strings with a single character for
// each element.
// Y indicates the element must be present.
// N indicates the element must not be present.
// X indicates the element is optional.
// The entire permission string being NULL indicates that the whole thing is
// not allowed.

#define ENTITY_MAIN 0
#define ENTITY_FIELD 2
#define ENTITY_CAP 4
#define ENTITY_C_API 6

// Index by DEF_<ENTITY>
static const permission_def_t _entity_def[DEF_ENTITY_COUNT] =
{ //                           Main
  //                           | field
  //                           | | cap
  //                           | | | c_api
  { "actor",                  "X X N X" },
  { "class",                  "N X X N" },
  { "struct",                 "N X X N" },
  { "primitive",              "N N N N" },
  { "trait",                  "N N X N" },
  { "interface",              "N N X N" },
  { "type alias",             "N N N N" }
};

#define METHOD_CAP 0
#define METHOD_BARE 2
#define METHOD_RETURN 4
#define METHOD_ERROR 6
#define METHOD_BODY 8

// Index by DEF_<ENTITY> + DEF_<METHOD>
static const permission_def_t _method_def[DEF_METHOD_COUNT] =
{ //                           cap
  //                           | bare
  //                           | | return
  //                           | | | error
  //                           | | | | body
  { "actor function",         "X X X X Y" },
  { "class function",         "X X X X Y" },
  { "struct function",        "X X X X Y" },
  { "primitive function",     "X X X X Y" },
  { "trait function",         "X X X X X" },
  { "interface function",     "X X X X X" },
  { "type alias function",    NULL },
  { "actor behaviour",        "N N N N Y" },
  { "class behaviour",        NULL },
  { "struct behaviour",       NULL },
  { "primitive behaviour",    NULL },
  { "trait behaviour",        "N N N N X" },
  { "interface behaviour",    "N N N N X" },
  { "type alias behaviour",   NULL },
  { "actor constructor",      "N N N N Y" },
  { "class constructor",      "X N N X Y" },
  { "struct constructor",     "X N N X Y" },
  { "primitive constructor",  "N N N X Y" },
  { "trait constructor",      "X N N X N" },
  { "interface constructor",  "X N N X N" },
  { "type alias constructor", NULL }
};


static bool is_expr_infix(token_id id)
{
  switch(id)
  {
    case TK_AND:
    case TK_OR:
    case TK_XOR:
    case TK_PLUS:
    case TK_MINUS:
    case TK_MULTIPLY:
    case TK_DIVIDE:
    case TK_REM:
    case TK_MOD:
    case TK_PLUS_TILDE:
    case TK_MINUS_TILDE:
    case TK_MULTIPLY_TILDE:
    case TK_DIVIDE_TILDE:
    case TK_REM_TILDE:
    case TK_MOD_TILDE:
    case TK_LSHIFT:
    case TK_RSHIFT:
    case TK_LSHIFT_TILDE:
    case TK_RSHIFT_TILDE:
    case TK_IS:
    case TK_ISNT:
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GE:
    case TK_GT:
    case TK_EQ_TILDE:
    case TK_NE_TILDE:
    case TK_LT_TILDE:
    case TK_LE_TILDE:
    case TK_GE_TILDE:
    case TK_GT_TILDE:
    case TK_UNIONTYPE:
    case TK_ISECTTYPE:
      return true;

    default:
      return false;
  }
}


// Check whether the given node is a valid provides type
static bool check_provides_type(pass_opt_t* opt, ast_t* type,
  const char* description)
{
  pony_assert(type != NULL);
  pony_assert(description != NULL);

  switch(ast_id(type))
  {
    case TK_NOMINAL:
    {
      AST_GET_CHILDREN(type, ignore0, ignore1, ignore2, cap, ephemeral);

      if(ast_id(cap) != TK_NONE)
      {
        ast_error(opt->check.errors, cap,
          "can't specify a capability in a provides type");
        return false;
      }

      if(ast_id(ephemeral) != TK_NONE)
      {
        ast_error(opt->check.errors, ephemeral,
          "can't specify ephemeral in a provides type");
        return false;
      }

      return true;
    }

    case TK_PROVIDES:
    case TK_ISECTTYPE:
      // Check all our children are also legal
      for(ast_t* p = ast_child(type); p != NULL; p = ast_sibling(p))
      {
        if(!check_provides_type(opt, p, description))
          return false;
      }

      return true;

    default:
      ast_error(opt->check.errors, type, "invalid %s type. Can only be "
        "interfaces, traits and intersects of those.", description);
      return false;
  }
}


// Check permission for one specific element of a method or entity
static bool check_permission(pass_opt_t* opt, const permission_def_t* def,
  int element, ast_t* actual, const char* context, ast_t* report_at)
{
  pony_assert(def != NULL);
  pony_assert(actual != NULL);
  pony_assert(context != NULL);

  char permission = def->permissions[element];

  pony_assert(permission == 'Y' || permission == 'N' || permission == 'X');

  if(permission == 'N' && ast_id(actual) != TK_NONE)
  {
    ast_error(opt->check.errors, actual, "%s cannot specify %s",
      def->desc, context);
    return false;
  }

  if(permission == 'Y' && ast_id(actual) == TK_NONE)
  {
    ast_error(opt->check.errors, report_at, "%s must specify %s",
      def->desc, context);
    return false;
  }

  return true;
}


// Check whether the given method has any illegal parts
static bool check_method(pass_opt_t* opt, ast_t* ast, int method_def_index)
{
  pony_assert(ast != NULL);
  pony_assert(method_def_index >= 0 && method_def_index < DEF_METHOD_COUNT);
  bool r = true;

  const permission_def_t* def = &_method_def[method_def_index];

  if(def->permissions == NULL)
  {
    ast_error(opt->check.errors, ast, "%ss are not allowed", def->desc);
    return false;
  }

  AST_GET_CHILDREN(ast, cap, id, type_params, params, return_type,
    error, body, docstring);

  if(ast_id(cap) == TK_AT)
  {
    if(!check_permission(opt, def, METHOD_BARE, cap, "bareness", cap))
      r = false;
  } else if(!check_permission(opt, def, METHOD_CAP, cap, "receiver capability",
    cap))
  {
    r = false;
  }

  if(!check_id_method(opt, id))
    r = false;

  if(!check_permission(opt, def, METHOD_RETURN, return_type, "return type",
    ast))
    r = false;

  if(!check_permission(opt, def, METHOD_ERROR, error, "?", ast))
    r = false;

  if(!check_permission(opt, def, METHOD_BODY, body, "body", ast))
    r = false;

  if(ast_id(docstring) == TK_STRING)
  {
    if(ast_id(body) != TK_NONE)
    {
      ast_error(opt->check.errors, docstring,
        "methods with bodies must put docstrings in the body");
      r = false;
    }
  }

  return r;
}


// Check whether the given entity members are legal in their entity
static bool check_members(pass_opt_t* opt, ast_t* members, int entity_def_index)
{
  pony_assert(members != NULL);
  pony_assert(entity_def_index >= 0 && entity_def_index < DEF_ENTITY_COUNT);
  bool r = true;

  const permission_def_t* def = &_entity_def[entity_def_index];
  ast_t* member = ast_child(members);

  while(member != NULL)
  {
    switch(ast_id(member))
    {
      case TK_FLET:
      case TK_FVAR:
      case TK_EMBED:
      {
        if(def->permissions[ENTITY_FIELD] == 'N')
        {
          ast_error(opt->check.errors, member,
            "Can't have fields in %s", def->desc);
          r = false;
        }

        if((ast_id(ast_parent(members)) == TK_OBJECT) && \
          (ast_id(ast_childidx(member, 2)) == TK_NONE))
        {
          ast_error(opt->check.errors, member,
            "object literal fields must be initialized");
          r = false;
        }

        if(!check_id_field(opt, ast_child(member)))
          r = false;

        break;
      }

      case TK_NEW:
        if(!check_method(opt, member, entity_def_index + DEF_NEW))
          r = false;
        break;

      case TK_BE:
      {
        if(!check_method(opt, member, entity_def_index + DEF_BE))
          r = false;
        break;
      }

      case TK_FUN:
      {
        if(!check_method(opt, member, entity_def_index + DEF_FUN))
          r = false;
        break;
      }

      default:
        pony_assert(0);
        return false;
    }

    member = ast_sibling(member);
  }

  return r;
}


// Check whether the given entity has illegal parts
static ast_result_t syntax_entity(pass_opt_t* opt, ast_t* ast,
  int entity_def_index)
{
  pony_assert(ast != NULL);
  pony_assert(entity_def_index >= 0 && entity_def_index < DEF_ENTITY_COUNT);
  ast_result_t r = AST_OK;

  const permission_def_t* def = &_entity_def[entity_def_index];
  AST_GET_CHILDREN(ast, id, typeparams, defcap, provides, members, c_api);

  // Check if we're called Main
  if(ast_name(id) == stringtab("Main"))
  {
    if(ast_id(typeparams) != TK_NONE)
    {
      ast_error(opt->check.errors, typeparams,
        "the Main actor cannot have type parameters");
      r = AST_ERROR;
    }

    if(def->permissions[ENTITY_MAIN] == 'N')
    {
      ast_error(opt->check.errors, ast, "Main must be an actor");
      r = AST_ERROR;
    }
  }

  if(!check_id_type(opt, id, def->desc))
    r = AST_ERROR;

  if(!check_permission(opt, def, ENTITY_CAP, defcap, "default capability",
    defcap))
    r = AST_ERROR;

  if(!check_permission(opt, def, ENTITY_C_API, c_api, "C api", c_api))
    r = AST_ERROR;

  if(ast_id(c_api) == TK_AT)
  {
    if(ast_id(typeparams) != TK_NONE)
    {
      ast_error(opt->check.errors, typeparams,
        "generic actor cannot specify C api");
      r = AST_ERROR;
    }
  }

  if(entity_def_index != DEF_TYPEALIAS)
  {
    // Check referenced traits
    if(ast_id(provides) != TK_NONE)
    {
      if(ast_has_annotation(ast, "nosupertype"))
      {
        ast_error(opt->check.errors, provides,
          "a 'nosupertype' type cannot specify a provides list");
        r = AST_ERROR;
      } else if(!check_provides_type(opt, provides, "provides")) {
        r = AST_ERROR;
      }
    }
  }
  else
  {
    // Check for a type alias
    if(ast_id(provides) == TK_NONE)
    {
      ast_error(opt->check.errors, provides,
        "a type alias must specify a type");
      r = AST_ERROR;
    }
  }

  // Check for illegal members
  if(!check_members(opt, members, entity_def_index))
    r = AST_ERROR;

  return r;
}


static ast_result_t syntax_thistype(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  ast_t* parent = ast_parent(ast);
  pony_assert(parent != NULL);
  ast_result_t r = AST_OK;

  if(ast_id(parent) != TK_ARROW)
  {
    ast_error(opt->check.errors, ast,
      "in a type, 'this' can only be used as a viewpoint");
    r = AST_ERROR;
  }

  if(opt->check.frame->method == NULL)
  {
    ast_error(opt->check.errors, ast,
      "can only use 'this' for a viewpoint in a method");
    r = AST_ERROR;
  } else {
    ast_t* cap = ast_child(opt->check.frame->method);

    switch(ast_id(cap))
    {
      case TK_BOX:
      case TK_NONE:
        break;

      default:
        ast_error(opt->check.errors, ast,
          "can only use 'this' for a viewpoint in a box function");
        r = AST_ERROR;
    }
  }

  return r;
}


static ast_result_t syntax_arrow(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  AST_GET_CHILDREN(ast, left, right);

  if((opt->check.frame->constraint != NULL) ||
    (opt->check.frame->iftype_constraint != NULL))
  {
    ast_error(opt->check.errors, ast,
      "arrow types can't be used as type constraints");
    return AST_ERROR;
  }

  switch(ast_id(right))
  {
    case TK_THISTYPE:
      ast_error(opt->check.errors, ast,
        "'this' cannot appear to the right of a viewpoint");
      return AST_ERROR;

    case TK_ISO:
    case TK_TRN:
    case TK_REF:
    case TK_VAL:
    case TK_BOX:
    case TK_TAG:
      ast_error(opt->check.errors, ast,
        "refcaps cannot appear to the right of a viewpoint");
      return AST_ERROR;

    default: {}
  }

  return AST_OK;
}


static ast_result_t syntax_tupletype(pass_opt_t* opt, ast_t* ast)
{
  if(opt->check.frame->constraint != NULL)
  {
    ast_error(opt->check.errors, ast,
      "tuple types can't be used as type constraints");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_nominal(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  AST_GET_CHILDREN(ast, package, name, typeargs, cap, eph);

  if(!is_name_dontcare(ast_name(name)))
    return AST_OK;

  ast_result_t r = AST_OK;

  if(ast_id(package) != TK_NONE)
  {
    ast_error(opt->check.errors, package,
      "'_' cannot be in a package");
    r = AST_ERROR;
  }

  if(ast_id(typeargs) != TK_NONE)
  {
    ast_error(opt->check.errors, typeargs,
      "'_' cannot have generic arguments");
    r = AST_ERROR;
  }

  if(ast_id(cap) != TK_NONE)
  {
    ast_error(opt->check.errors, cap,
      "'_' cannot specify capability");
    r = AST_ERROR;
  }

  if(ast_id(eph) != TK_NONE)
  {
    ast_error(opt->check.errors, eph,
      "'_' cannot specify capability modifier");
    r = AST_ERROR;
  }

  return r;
}


static ast_result_t syntax_match(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);

  // The last case must have a body
  ast_t* cases = ast_childidx(ast, 1);
  pony_assert(cases != NULL);
  pony_assert(ast_id(cases) == TK_CASES);

  ast_t* case_ast = ast_child(cases);

  if(case_ast == NULL)  // There are no bodies
    return AST_OK;

  while(ast_sibling(case_ast) != NULL)
    case_ast = ast_sibling(case_ast);

  ast_t* body = ast_childidx(case_ast, 2);

  if(ast_id(body) == TK_NONE)
  {
    ast_error(opt->check.errors,
      case_ast, "Last case in match must have a body");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_ffi(pass_opt_t* opt, ast_t* ast,
  bool is_declaration)
{
  pony_assert(ast != NULL);
  ast_result_t r = AST_OK;

  AST_GET_CHILDREN(ast, id, typeargs, ffi_args, ffi_named_args);
  // We don't check FFI names are legal, if the lexer allows it so do we
  if((ast_child(typeargs) == NULL && is_declaration) ||
    ast_childidx(typeargs, 1) != NULL)
  {
    ast_error(opt->check.errors, typeargs,
      "FFI functions must specify a single return type");
    r = AST_ERROR;
  }

  for(ast_t* p = ast_child(ffi_args); p != NULL; p = ast_sibling(p))
  {
    if(ast_id(p) == TK_PARAM)
    {
      ast_t* def_val = ast_childidx(p, 2);
      pony_assert(def_val != NULL);

      if(ast_id(def_val) != TK_NONE)
      {
        ast_error(opt->check.errors, def_val,
          "FFIs parameters cannot have default values");
        r = AST_ERROR;
      }
    }
  }

  if(ast_id(ffi_named_args) != TK_NONE)
  {
    ast_error(opt->check.errors, ffi_named_args,
      "FFIs cannot take named arguments");
    r = AST_ERROR;
  }

  return r;
}


static ast_result_t syntax_ffi_decl(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_FFIDECL);
  return syntax_ffi(opt, ast, true);
}

static ast_result_t syntax_ffi_call(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_FFICALL);
  ast_result_t r = AST_OK;

  ast_t* in_method = opt->check.frame->method;
  if((in_method != NULL) &&
    (ast_id(in_method) == TK_BE || ast_id(in_method) == TK_FUN))
  {
    ast_t* parent = ast_parent(ast);
    switch(ast_id(opt->check.frame->method))
    {
    case TK_BE:
    case TK_FUN:
      while(parent != NULL)
      {
        switch(ast_id(parent)) {
        case TK_INTERFACE:
        case TK_TRAIT:
          ast_error(opt->check.errors, ast,
            "Can't call an FFI function in a default method or behavior");
          r = AST_ERROR;
          break;
        default: {}
        }

        parent = ast_parent(parent);
      }
    default: {}
    }
  }

  if(syntax_ffi(opt, ast, false) == AST_ERROR)
    r = AST_ERROR;

  return r;
}


static ast_result_t syntax_ellipsis(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  ast_result_t r = AST_OK;

  ast_t* fn = ast_parent(ast_parent(ast));
  pony_assert(fn != NULL);

  if(ast_id(fn) != TK_FFIDECL)
  {
    ast_error(opt->check.errors, ast,
      "... may only appear in FFI declarations");
    r = AST_ERROR;
  }

  if(ast_sibling(ast) != NULL)
  {
    ast_error(opt->check.errors, ast, "... must be the last parameter");
    r = AST_ERROR;
  }

  return r;
}


static ast_result_t syntax_infix_expr(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  AST_GET_CHILDREN(ast, left, right);

  token_id op = ast_id(ast);

  pony_assert(left != NULL);
  token_id left_op = ast_id(left);
  bool left_clash = (left_op != op) && is_expr_infix(left_op) &&
    !ast_checkflag(left, AST_FLAG_IN_PARENS);

  pony_assert(right != NULL);
  token_id right_op = ast_id(right);
  bool right_clash = (right_op != op) && is_expr_infix(right_op) &&
    !ast_checkflag(right, AST_FLAG_IN_PARENS);

  if(left_clash || right_clash)
  {
    ast_error(opt->check.errors, ast,
      "Operator precedence is not supported. Parentheses required.");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_consume(pass_opt_t* opt, ast_t* ast)
{
  AST_GET_CHILDREN(ast, cap, term);

  switch(ast_id(term))
  {
    case TK_THIS:
    case TK_REFERENCE:
      return AST_OK;
    case TK_DOT: {
      AST_GET_CHILDREN(term, left, right);
      if (ast_id(left) != TK_CALL && ast_id(left) != TK_SEQ)
      {
        return AST_OK;
      }
    }
    default: {}
  }

  ast_error(opt->check.errors, term,
    "Consume expressions must specify an identifier or field");
  return AST_ERROR;
}


static ast_result_t syntax_return(pass_opt_t* opt, ast_t* ast,
  size_t max_value_count)
{
  pony_assert(ast != NULL);

  ast_t* value_seq = ast_child(ast);
  pony_assert(ast_id(value_seq) == TK_SEQ || ast_id(value_seq) == TK_NONE);
  size_t value_count = ast_childcount(value_seq);

  if(value_count > max_value_count)
  {
    ast_error(opt->check.errors,
      ast_childidx(value_seq, max_value_count), "Unreachable code");
    return AST_ERROR;
  }

  ast_t* parent = ast_parent(ast);
  ast_t* current = ast;
  while(ast_id(parent) == TK_SEQ)
  {
    if(ast_sibling(current) != NULL)
    {
      ast_error(opt->check.errors,
        ast_sibling(current), "Unreachable code");
      return AST_ERROR;
    }
    current = parent;
    parent = ast_parent(parent);
  }

  if(ast_id(ast) == TK_RETURN)
  {
    if(opt->check.frame->method_body == NULL)
    {
      ast_error(opt->check.errors, ast, "return must occur in a method body");
      return AST_ERROR;
    }

    if(value_count > 0)
    {
      if (ast_id(opt->check.frame->method) == TK_BE ||
        ast_id(opt->check.frame->method) == TK_NEW)
      {
        bool should_error = true;
        ast_t* pparent = parent;

        switch(ast_id(opt->check.frame->method))
        {
          case TK_BE:
          case TK_NEW:
            while (pparent != NULL)
            {
              switch(ast_id(pparent))
              {
                case TK_LAMBDA:
                  should_error = false;
                case TK_BE:
                case TK_NEW:
                  break;
                default: {}
              }

              pparent = ast_parent(pparent);
            }
          default: {}
        }

        if (should_error)
        {
          ast_error(opt->check.errors, ast,
            "A return in a constructor or a behaviour can't return a value");
          return AST_ERROR;
        }
      }
    }
  }

  return AST_OK;
}


static ast_result_t syntax_semi(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_parent(ast) != NULL);
  pony_assert(ast_id(ast_parent(ast)) == TK_SEQ);

  if(ast_checkflag(ast, AST_FLAG_BAD_SEMI))
  {
    ast_error(opt->check.errors, ast, "Unexpected semicolon, only use to "
      "separate expressions on the same line");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_local(pass_opt_t* opt, ast_t* ast)
{
  if(!check_id_local(opt, ast_child(ast)))
    return AST_ERROR;

  return AST_OK;
}


static ast_result_t syntax_embed(pass_opt_t* opt, ast_t* ast)
{
  if(ast_id(ast_parent(ast)) != TK_MEMBERS)
  {
    ast_error(opt->check.errors, ast, "Local variables cannot be embedded");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_type_param(pass_opt_t* opt, ast_t* ast)
{

  if(!check_id_type_param(opt, ast_child(ast)))
    return AST_ERROR;

  return AST_OK;
}


static const char* const _illegal_flags[] =
{
  "ndebug",
  "unknown_os",
  "unknown_size",
  NULL  // Terminator.
};


// Check the given ast is a valid ifdef condition.
// The context parameter is for error messages and should be a literal string
// such as "ifdef condition" or "use guard".
static bool syntax_ifdef_cond(pass_opt_t* opt, ast_t* ast, const char* context)
{
  pony_assert(ast != NULL);
  pony_assert(context != NULL);

  switch(ast_id(ast))
  {

    case TK_AND:
    case TK_OR:
    case TK_NOT:
      // Valid node.
      break;

    case TK_NONE:
      // Valid because we have an optional TK_QUESTION in TK_AND and TK_OR.
      break;

    case TK_STRING:
    {
      // Check user flag is not also a platform, or outlawed, flags
      const char* name = ast_name(ast);

      // Create an all lower case version of the name for comparisons.
      size_t len = strlen(name) + 1;
      char* lower_case = (char*)ponyint_pool_alloc_size(len);

      for(size_t i = 0; i < len; i++)
        lower_case[i] = (char)tolower(name[i]);

      bool r = true;
      bool result;
      if(os_is_target(lower_case, true, &result, opt))
        r = false;

      for(int i = 0; _illegal_flags[i] != NULL; i++)
        if(strcmp(lower_case, _illegal_flags[i]) == 0)
          r = false;

      ponyint_pool_free_size(len, lower_case);

      if(!r)
      {
        ast_error(opt->check.errors, ast,
          "\"%s\" is not a valid user build flag\n", name);
        return false;
      }

      // TODO: restrict case?
      break;
    }

    case TK_REFERENCE:
    {
      const char* name = ast_name(ast_child(ast));
      bool result;
      if(!os_is_target(name, true, &result, opt))
      {
        ast_error(opt->check.errors, ast,
          "\"%s\" is not a valid platform flag\n", name);
        return false;
      }

      // Don't recurse into children, that'll hit the ID node
      return true;
    }

    case TK_SEQ:
      if(ast_childcount(ast) != 1)
      {
        ast_error(opt->check.errors, ast,
          "Sequence not allowed in %s", context);
        return false;
      }

      break;

    default:
      ast_error(opt->check.errors, ast, "Invalid %s", context);
      return false;
  }

  for(ast_t* p = ast_child(ast); p != NULL; p = ast_sibling(p))
  {
    if(!syntax_ifdef_cond(opt, p, context))
      return false;
  }

  return true;
}


static ast_result_t syntax_ifdef(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);

  if(!syntax_ifdef_cond(opt, ast_child(ast), "ifdef condition"))
    return AST_ERROR;

  return AST_OK;
}


static ast_result_t syntax_use(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast != NULL);
  AST_GET_CHILDREN(ast, id, url, guard);

  if(ast_id(id) != TK_NONE && !check_id_package(opt, id))
    return AST_ERROR;

  if(ast_id(guard) != TK_NONE && !syntax_ifdef_cond(opt, guard, "use guard"))
    return AST_ERROR;

  return AST_OK;
}


static ast_result_t syntax_lambda_capture(pass_opt_t* opt, ast_t* ast)
{
  AST_GET_CHILDREN(ast, name, type, value);

  if(ast_id(type) != TK_NONE && ast_id(value) == TK_NONE)
  {
    ast_error(opt->check.errors, ast, "value missing for lambda expression "
      "capture (cannot specify type without value)");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_barelambdatype(pass_opt_t* opt, ast_t* ast)
{
  AST_GET_CHILDREN(ast, fun_cap, id, typeparams, params, return_type, partial,
    obj_cap, obj_mod);

  if(ast_id(fun_cap) != TK_NONE)
  {
    ast_error(opt->check.errors, fun_cap, "a bare lambda cannot specify a "
      "receiver capability");
    return AST_ERROR;
  }

  if(ast_id(typeparams) != TK_NONE)
  {
    ast_error(opt->check.errors, typeparams, "a bare lambda cannot specify "
      "type parameters");
    return AST_ERROR;
  }

  switch(ast_id(obj_cap))
  {
    case TK_VAL:
    case TK_NONE:
      break;

    default:
      ast_error(opt->check.errors, obj_cap, "a bare lambda can only have a "
        "'val' capability");
      return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_compile_intrinsic(pass_opt_t* opt, ast_t* ast)
{
  ast_t* parent = ast_parent(ast);
  pony_assert(ast_id(parent) == TK_SEQ);

  ast_t* method = ast_parent(parent);

  switch(ast_id(method))
  {
    case TK_NEW:
    case TK_BE:
    case TK_FUN:
      // OK
      break;

    default:
      ast_error(opt->check.errors, ast,
        "a compile intrinsic must be a method body");
      return AST_ERROR;
  }

  ast_t* child = ast_child(parent);

  // Allow a docstring before the compile_instrinsic.
  if(ast_id(child) == TK_STRING)
    child = ast_sibling(child);

  // Compile intrinsic has a value child, but it must be empty
  ast_t* value = ast_child(ast);

  if(child != ast || ast_sibling(child) != NULL || ast_id(value) != TK_NONE)
  {
    ast_error(opt->check.errors, ast,
      "a compile intrinsic must be the entire body");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_compile_error(pass_opt_t* opt, ast_t* ast)
{
  ast_t* parent = ast_parent(ast);
  pony_assert(ast_id(parent) == TK_SEQ);

  if(ast_id(ast_parent(parent)) != TK_IFDEF)
  {
    ast_error(opt->check.errors, ast, "a compile error must be in an ifdef");
    return AST_ERROR;
  }

  // AST must be of the form:
  // (compile_error (seq "Reason"))
  ast_t* reason_seq = ast_child(ast);

  if(ast_id(reason_seq) != TK_SEQ ||
    ast_id(ast_child(reason_seq)) != TK_STRING)
  {
    ast_error(opt->check.errors, ast,
      "a compile error must have a string literal reason for the error");
    return AST_ERROR;
  }

  ast_t* child = ast_child(parent);

  if((child != ast) || (ast_sibling(child) != NULL) ||
    (ast_childcount(reason_seq) != 1))
  {
    ast_error(opt->check.errors, ast,
      "a compile error must be the entire ifdef clause");
    return AST_ERROR;
  }

  return AST_OK;
}


static ast_result_t syntax_lambda(pass_opt_t* opt, ast_t* ast)
{
  pony_assert((ast_id(ast) == TK_LAMBDA) || (ast_id(ast) == TK_BARELAMBDA));
  AST_GET_CHILDREN(ast, receiver_cap, name, t_params, params, captures,
    ret_type, raises, body, reference_cap);
  bool r = true;

  switch(ast_id(ret_type))
  {
    case TK_ISO:
    case TK_TRN:
    case TK_REF:
    case TK_VAL:
    case TK_BOX:
    case TK_TAG:
    {
      ast_error(opt->check.errors, ret_type, "lambda return type: %s",
        ast_print_type(ret_type));
      ast_error_continue(opt->check.errors, ret_type, "lambda return type "
        "cannot be capability");
      r = false;
    }
    default: {}
  }

  if(ast_id(ast) == TK_BARELAMBDA)
  {
    if(ast_id(receiver_cap) != TK_NONE)
    {
      ast_error(opt->check.errors, receiver_cap, "a bare lambda cannot specify "
        "a receiver capability");
      r = false;
    }

    if(ast_id(t_params) != TK_NONE)
    {
      ast_error(opt->check.errors, t_params, "a bare lambda cannot specify "
        "type parameters");
      r = false;
    }

    if(ast_id(captures) != TK_NONE)
    {
      ast_error(opt->check.errors, captures, "a bare lambda cannot specify "
        "captures");
      r = false;
    }

    switch(ast_id(reference_cap))
    {
      case TK_VAL:
      case TK_NONE:
        break;

      default:
        ast_error(opt->check.errors, reference_cap, "a bare lambda can only "
          "have a 'val' capability");
        r = false;
    }
  }

  ast_t* capture = ast_child(captures);
  while(capture != NULL)
  {
    if(ast_id(capture) == TK_THIS)
    {
      ast_error(opt->check.errors, capture,
        "use a named capture to capture 'this'");
      r = false;
    }
    capture = ast_sibling(capture);
  }

  return r ? AST_OK : AST_ERROR;
}


static ast_result_t syntax_object(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_OBJECT);
  AST_GET_CHILDREN(ast, cap, provides, members);

  // Check for illegal members - even though object literals can be non-actors,
  // we use DEF_ACTOR because the permissions are close enough for our purposes.
  if(!check_members(opt, members, DEF_ACTOR))
    return AST_ERROR;

  if(ast_id(provides) != TK_NONE && !check_provides_type(opt, provides, "provides"))
    return AST_ERROR;

  return AST_OK;
}


static ast_result_t syntax_fun(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_FUN);
  AST_GET_CHILDREN(ast, cap, id, typeparams, params, type, can_error, body);
  switch(ast_id(type))
  {
    case TK_ISO:
    case TK_TRN:
    case TK_REF:
    case TK_VAL:
    case TK_BOX:
    case TK_TAG:
    {
      ast_error(opt->check.errors, type, "function return type: %s",
        ast_print_type(type));
      ast_error_continue(opt->check.errors, type, "function return type "
        "cannot be capability");
      return AST_ERROR;
    }
    default: {}
  }

  return AST_OK;
}


static ast_result_t syntax_cap(pass_opt_t* opt, ast_t* ast)
{
  switch(ast_id(ast_parent(ast)))
  {
    case TK_NOMINAL:
    case TK_ARROW:
    case TK_OBJECT:
    case TK_LAMBDA:
    case TK_BARELAMBDA:
    case TK_RECOVER:
    case TK_CONSUME:
    case TK_FUN:
    case TK_BE:
    case TK_NEW:
    case TK_TYPE:
    case TK_INTERFACE:
    case TK_TRAIT:
    case TK_PRIMITIVE:
    case TK_STRUCT:
    case TK_CLASS:
    case TK_ACTOR:
    case TK_LAMBDATYPE:
    case TK_BARELAMBDATYPE:
      return AST_OK;

    default: {}
  }

  ast_error(opt->check.errors, ast, "a type cannot be only a capability");
  return AST_ERROR;
}


static ast_result_t syntax_cap_set(pass_opt_t* opt, ast_t* ast)
{
  // Cap sets can only appear in type parameter constraints.
  if((opt->check.frame->constraint == NULL) &&
    (opt->check.frame->iftype_constraint == NULL))
  {
    ast_error(opt->check.errors, ast,
      "a capability set can only appear in a type constraint");
    return AST_ERROR;
  }

  return AST_OK;
}


static bool check_annotation_location(pass_opt_t* opt, ast_t* ast,
  ast_t* loc, const char* str)
{
  if((strcmp(str, "likely") == 0) || (strcmp(str, "unlikely") == 0))
  {
    ast_t* parent = ast_parent(ast);

    switch(ast_id(parent))
    {
      case TK_IF:
      case TK_WHILE:
      case TK_CASE:
        break;

      default:
      {
        ast_t* grandparent = ast_parent(parent);
        if((ast_id(grandparent) == TK_REPEAT) &&
          (ast_childidx(grandparent, 1) == parent))
          break;

        ast_error(opt->check.errors, loc,
          "a '%s' annotation can only appear on the condition of an if, "
          "while, or until, or on the case of a match", str);
        return false;
      }
    }
  } else if(strcmp(str, "packed") == 0) {
    if(ast_id(ast_parent(ast)) != TK_STRUCT)
    {
      ast_error(opt->check.errors, loc,
        "a 'packed' annotation can only appear on a struct declaration");
      return false;
    }
  } else if(strcmp(str, "nosupertype") == 0) {
    switch(ast_id(ast_parent(ast)))
    {
      case TK_CLASS:
      case TK_ACTOR:
      case TK_PRIMITIVE:
      case TK_STRUCT:
        break;

      default:
        ast_error(opt->check.errors, loc,
          "a 'nosupertype' annotation can only appear on a concrete type "
          "declaration");
        return false;
    }
  } else if(strcmp(str, "nodoc") == 0) {
    switch(ast_id(ast_parent(ast)))
    {
      case TK_ACTOR:
      case TK_CLASS:
      case TK_STRUCT:
      case TK_PRIMITIVE:
      case TK_TRAIT:
      case TK_INTERFACE:
      case TK_NEW:
      case TK_FUN:
      case TK_BE:
        break;

      default:
        ast_error(opt->check.errors, loc,
          "'nodoc' annotation isn't valid here");

        return false;
    }
  }

  return true;
}


static ast_result_t syntax_annotation(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_ANNOTATION);

  const char ponyint[] = "ponyint";
  ast_result_t ok = AST_OK;

  for(ast_t* child = ast_child(ast); child != NULL; child = ast_sibling(child))
  {
    const char* str = ast_name(child);

    if((strlen(str) >= (sizeof ponyint - 1)) &&
      (strncmp(str, ponyint, sizeof ponyint - 1) == 0))
    {
      ast_error(opt->check.errors, child,
        "annotations starting with 'ponyint' are reserved for internal use");
      ok = AST_ERROR;
      continue;
    }

    if(!check_annotation_location(opt, ast, child, str))
      ok = AST_ERROR;
  }

  return ok;
}


static ast_result_t syntax_as(pass_opt_t* opt, ast_t* ast)
{
  pony_assert(ast_id(ast) == TK_AS);
  AST_GET_CHILDREN(ast, expr);

  switch (ast_id(expr))
  {
    case TK_INT:
    case TK_FLOAT:
      ast_error(opt->check.errors, expr,
        "Cannot cast uninferred numeric literal");
      ast_error_continue(opt->check.errors, expr,
        "To give a numeric literal a specific type, "
        "use the constructor of that numeric type");
      return AST_ERROR;

    default: break;
  }

  return AST_OK;
}


ast_result_t pass_syntax(ast_t** astp, pass_opt_t* options)
{
  pony_assert(astp != NULL);
  ast_t* ast = *astp;
  pony_assert(ast != NULL);

  token_id id = ast_id(ast);
  ast_result_t r = AST_OK;

  switch(id)
  {
    case TK_SEMI:       r = syntax_semi(options, ast); break;
    case TK_TYPE:       r = syntax_entity(options, ast, DEF_TYPEALIAS); break;
    case TK_PRIMITIVE:  r = syntax_entity(options, ast, DEF_PRIMITIVE); break;
    case TK_STRUCT:     r = syntax_entity(options, ast, DEF_STRUCT); break;
    case TK_CLASS:      r = syntax_entity(options, ast, DEF_CLASS); break;
    case TK_ACTOR:      r = syntax_entity(options, ast, DEF_ACTOR); break;
    case TK_TRAIT:      r = syntax_entity(options, ast, DEF_TRAIT); break;
    case TK_INTERFACE:  r = syntax_entity(options, ast, DEF_INTERFACE); break;
    case TK_THISTYPE:   r = syntax_thistype(options, ast); break;
    case TK_ARROW:      r = syntax_arrow(options, ast); break;
    case TK_TUPLETYPE:  r = syntax_tupletype(options, ast); break;
    case TK_NOMINAL:    r = syntax_nominal(options, ast); break;
    case TK_MATCH:      r = syntax_match(options, ast); break;
    case TK_FFIDECL:    r = syntax_ffi_decl(options, ast); break;
    case TK_FFICALL:    r = syntax_ffi_call(options, ast); break;
    case TK_ELLIPSIS:   r = syntax_ellipsis(options, ast); break;
    case TK_CONSUME:    r = syntax_consume(options, ast); break;
    case TK_RETURN:
    case TK_BREAK:      r = syntax_return(options, ast, 1); break;
    case TK_CONTINUE:
    case TK_ERROR:      r = syntax_return(options, ast, 0); break;
    case TK_LET:
    case TK_VAR:        r = syntax_local(options, ast); break;
    case TK_EMBED:      r = syntax_embed(options, ast); break;
    case TK_TYPEPARAM:  r = syntax_type_param(options, ast); break;
    case TK_IFDEF:      r = syntax_ifdef(options, ast); break;
    case TK_USE:        r = syntax_use(options, ast); break;
    case TK_LAMBDACAPTURE:
                        r = syntax_lambda_capture(options, ast); break;
    case TK_BARELAMBDATYPE:
                        r = syntax_barelambdatype(options, ast); break;
    case TK_COMPILE_INTRINSIC:
                        r = syntax_compile_intrinsic(options, ast); break;
    case TK_COMPILE_ERROR:
                        r = syntax_compile_error(options, ast); break;

    case TK_ISO:
    case TK_TRN:
    case TK_REF:
    case TK_VAL:
    case TK_BOX:
    case TK_TAG:        r = syntax_cap(options, ast); break;

    case TK_LAMBDA:
    case TK_BARELAMBDA: r = syntax_lambda(options, ast); break;
    case TK_OBJECT:     r = syntax_object(options, ast); break;
    case TK_FUN:        r = syntax_fun(options, ast); break;

    case TK_CAP_READ:
    case TK_CAP_SEND:
    case TK_CAP_SHARE:
    case TK_CAP_ALIAS:
    case TK_CAP_ANY:    r = syntax_cap_set(options, ast); break;

    case TK_ANNOTATION: r = syntax_annotation(options, ast); break;

    case TK_VALUEFORMALARG:
    case TK_VALUEFORMALPARAM:
      ast_error(options->check.errors, ast,
        "Value formal parameters not yet supported");
      ast_error_continue(options->check.errors, ast_parent(ast),
        "Note that many functions including array indexing use the apply "
        "method rather than square brackets");
      r = AST_ERROR;
      break;

    case TK_CONSTANT:
      ast_error(options->check.errors, ast,
        "Compile time expressions not yet supported");
      r = AST_ERROR;
      break;

    case TK_AS:         r = syntax_as(options, ast); break;

    default: break;
  }

  if(is_expr_infix(id))
    r = syntax_infix_expr(options, ast);

  if(ast_checkflag(ast, AST_FLAG_MISSING_SEMI))
  {
    ast_error(options->check.errors, ast,
      "Use a semi colon to separate expressions on the same line");
    r = AST_ERROR;
  }

  ast_t* annotation = ast_annotation(ast);

  if(annotation != NULL)
  {
    ast_result_t r2 = ast_visit(&annotation, pass_syntax, NULL, options,
      PASS_SYNTAX);

    if(r2 > r)
      r = r2;
  }

  return r;
}
