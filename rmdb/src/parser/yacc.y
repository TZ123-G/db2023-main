%{
#include "ast.h"
#include "errors.h"
#include "yacc.tab.h"
#include <iostream>
#include <limits>
#include <memory>

int yylex(YYSTYPE *yylval, YYLTYPE *yylloc);

void yyerror(YYLTYPE *locp, const char* s) {
    std::cerr << "Parser Error at line " << locp->first_line << " column " << locp->first_column << ": " << s << std::endl;
}

using namespace ast;

int parse_positive_int_literal(const std::string &literal, const std::string &context_name) {
    try {
        size_t idx = 0;
        long long value = std::stoll(literal, &idx, 10);
        if (idx != literal.size() || value <= 0 || value > std::numeric_limits<int>::max()) {
            throw NumericOverflowError(literal, context_name);
        }
        return static_cast<int>(value);
    } catch (const std::invalid_argument &) {
        throw NumericOverflowError(literal, context_name);
    } catch (const std::out_of_range &) {
        throw NumericOverflowError(literal, context_name);
    }
}

int64_t parse_non_negative_bigint_literal(const std::string &literal, const std::string &context_name) {
    try {
        size_t idx = 0;
        long long value = std::stoll(literal, &idx, 10);
        if (idx != literal.size() || value < 0) {
            throw NumericOverflowError(literal, context_name);
        }
        return static_cast<int64_t>(value);
    } catch (const std::invalid_argument &) {
        throw NumericOverflowError(literal, context_name);
    } catch (const std::out_of_range &) {
        throw NumericOverflowError(literal, context_name);
    }
}
%}

// request a pure (reentrant) parser
%define api.pure full
// enable location in error handler
%locations
// enable verbose syntax error message
%define parse.error verbose

// keywords
%token SHOW TABLES CREATE TABLE DROP DESC INSERT INTO VALUES DELETE FROM ASC ORDER BY LIMIT
WHERE UPDATE SET SELECT INT BIGINT CHAR FLOAT DATETIME INDEX AND JOIN EXIT HELP TXN_BEGIN TXN_COMMIT TXN_ABORT TXN_ROLLBACK ORDER_BY
COUNT MAX MIN SUM AS
// non-keywords
%token LEQ NEQ GEQ T_EOF

// type-specific tokens
%token <sv_str> IDENTIFIER VALUE_STRING
%token <sv_str> VALUE_INT
%token <sv_float> VALUE_FLOAT

// specify types for non-terminal symbol
%type <sv_node> stmt dbStmt ddl dml txnStmt
%type <sv_field> field
%type <sv_fields> fieldList
%type <sv_type_len> type
%type <sv_comp_op> op
%type <sv_expr> expr
%type <sv_val> value
%type <sv_vals> valueList
%type <sv_str> tbName colName
%type <sv_strs> tableList colNameList
%type <sv_col> col aggregateArg
%type <sv_select_item> selectItem aggregate
%type <sv_select_items> selectItemList selector
%type <sv_agg_type> aggregateFunc
%type <sv_set_clause> setClause
%type <sv_set_clauses> setClauses
%type <sv_cond> condition
%type <sv_conds> whereClause optWhereClause
%type <sv_orderby> order_item
%type <sv_orderbys> order_clause opt_order_clause
%type <sv_orderby_dir> opt_asc_desc
%type <sv_bigint> opt_limit_clause
%type <sv_str> optAlias

%%
start:
        stmt ';'
    {
        parse_tree = $1;
        YYACCEPT;
    }
    |   HELP
    {
        parse_tree = std::make_shared<Help>();
        YYACCEPT;
    }
    |   EXIT
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    |   T_EOF
    {
        parse_tree = nullptr;
        YYACCEPT;
    }
    ;

stmt:
        dbStmt
    |   ddl
    |   dml
    |   txnStmt
    ;

txnStmt:
        TXN_BEGIN
    {
        $$ = std::make_shared<TxnBegin>();
    }
    |   TXN_COMMIT
    {
        $$ = std::make_shared<TxnCommit>();
    }
    |   TXN_ABORT
    {
        $$ = std::make_shared<TxnAbort>();
    }
    | TXN_ROLLBACK
    {
        $$ = std::make_shared<TxnRollback>();
    }
    ;

dbStmt:
        SHOW TABLES
    {
        $$ = std::make_shared<ShowTables>();
    }
    ;

ddl:
        CREATE TABLE tbName '(' fieldList ')'
    {
        $$ = std::make_shared<CreateTable>($3, $5);
    }
    |   DROP TABLE tbName
    {
        $$ = std::make_shared<DropTable>($3);
    }
    |   DESC tbName
    {
        $$ = std::make_shared<DescTable>($2);
    }
    |   CREATE INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<CreateIndex>($3, $5);
    }
    |   DROP INDEX tbName '(' colNameList ')'
    {
        $$ = std::make_shared<DropIndex>($3, $5);
    }
    ;

dml:
        INSERT INTO tbName VALUES '(' valueList ')'
    {
        $$ = std::make_shared<InsertStmt>($3, $6);
    }
    |   DELETE FROM tbName optWhereClause
    {
        $$ = std::make_shared<DeleteStmt>($3, $4);
    }
    |   UPDATE tbName SET setClauses optWhereClause
    {
        $$ = std::make_shared<UpdateStmt>($2, $4, $5);
    }
    |   SELECT selector FROM tableList optWhereClause opt_order_clause opt_limit_clause
    {
        $$ = std::make_shared<SelectStmt>($2, $4, $5, $6, $7);
    }
    ;

fieldList:
        field
    {
        $$ = std::vector<std::shared_ptr<Field>>{$1};
    }
    |   fieldList ',' field
    {
        $$.push_back($3);
    }
    ;

colNameList:
        colName
    {
        $$ = std::vector<std::string>{$1};
    }
    | colNameList ',' colName
    {
        $$.push_back($3);
    }
    ;

field:
        colName type
    {
        $$ = std::make_shared<ColDef>($1, $2);
    }
    ;

type:
        INT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_INT, sizeof(int));
    }
    |   BIGINT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_BIGINT, sizeof(int64_t));
    }
    |   CHAR '(' VALUE_INT ')'
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_STRING, parse_positive_int_literal($3, "CHAR"));
    }
    |   FLOAT
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_FLOAT, sizeof(float));
    }
    |   DATETIME
    {
        $$ = std::make_shared<TypeLen>(SV_TYPE_DATETIME, sizeof(int64_t));
    }
    ;

valueList:
        value
    {
        $$ = std::vector<std::shared_ptr<Value>>{$1};
    }
    |   valueList ',' value
    {
        $$.push_back($3);
    }
    ;

value:
        VALUE_INT
    {
        $$ = std::make_shared<IntLit>($1);
    }
    |   VALUE_FLOAT
    {
        $$ = std::make_shared<FloatLit>($1);
    }
    |   VALUE_STRING
    {
        $$ = std::make_shared<StringLit>($1);
    }
    ;

condition:
        col op expr
    {
        $$ = std::make_shared<BinaryExpr>($1, $2, $3);
    }
    ;

optWhereClause:
        /* epsilon */ { /* ignore*/ }
    |   WHERE whereClause
    {
        $$ = $2;
    }
    ;

whereClause:
        condition 
    {
        $$ = std::vector<std::shared_ptr<BinaryExpr>>{$1};
    }
    |   whereClause AND condition
    {
        $$.push_back($3);
    }
    ;

col:
        tbName '.' colName
    {
        $$ = std::make_shared<Col>($1, $3);
    }
    |   colName
    {
        $$ = std::make_shared<Col>("", $1);
    }
    ;

op:
        '='
    {
        $$ = SV_OP_EQ;
    }
    |   '<'
    {
        $$ = SV_OP_LT;
    }
    |   '>'
    {
        $$ = SV_OP_GT;
    }
    |   NEQ
    {
        $$ = SV_OP_NE;
    }
    |   LEQ
    {
        $$ = SV_OP_LE;
    }
    |   GEQ
    {
        $$ = SV_OP_GE;
    }
    ;

expr:
        value
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    |   col
    {
        $$ = std::static_pointer_cast<Expr>($1);
    }
    ;

setClauses:
        setClause
    {
        $$ = std::vector<std::shared_ptr<SetClause>>{$1};
    }
    |   setClauses ',' setClause
    {
        $$.push_back($3);
    }
    ;

setClause:
        colName '=' value
    {
        $$ = std::make_shared<SetClause>($1, $3);
    }
    ;

selector:
        '*'
    {
        $$ = {};
    }
    |   selectItemList
    ;

selectItemList:
        selectItem
    {
        $$ = std::vector<std::shared_ptr<SelectItem>>{$1};
    }
    |   selectItemList ',' selectItem
    {
        $$.push_back($3);
    }
    ;

selectItem:
        col
    {
        $$ = std::make_shared<SelectItem>($1);
    }
    |   aggregate
    ;

aggregate:
        aggregateFunc '(' aggregateArg ')' optAlias
    {
        $$ = std::make_shared<SelectItem>($1, $3, $3 == nullptr, $5);
    }
    ;

aggregateArg:
        col { $$ = $1; }
    |   '*' { $$ = nullptr; }
    ;

aggregateFunc:
        COUNT { $$ = SV_AGG_COUNT; }
    |   MAX   { $$ = SV_AGG_MAX; }
    |   MIN   { $$ = SV_AGG_MIN; }
    |   SUM   { $$ = SV_AGG_SUM; }
    ;

optAlias:
        /* epsilon */ { $$ = ""; }
    |   AS colName   { $$ = $2; }
    ;

tableList:
        tbName
    {
        $$ = std::vector<std::string>{$1};
    }
    |   tableList ',' tbName
    {
        $$.push_back($3);
    }
    |   tableList JOIN tbName
    {
        $$.push_back($3);
    }
    ;

opt_order_clause:
    ORDER BY order_clause      
    { 
        $$ = $3; 
    }
    |   /* epsilon */ { $$ = {}; }
    ;

order_clause:
      order_item
    { 
        $$ = std::vector<std::shared_ptr<OrderBy>>{$1};
    }
    | order_clause ',' order_item
    {
        $$.push_back($3);
    }
    ;

order_item:
      col  opt_asc_desc
    {
        $$ = std::make_shared<OrderBy>($1, $2);
    }
    ;

opt_limit_clause:
    LIMIT VALUE_INT
    {
        $$ = parse_non_negative_bigint_literal($2, "LIMIT");
    }
    | /* epsilon */
    {
        $$ = -1;
    }
    ;

opt_asc_desc:
    ASC          { $$ = OrderBy_ASC;     }
    |  DESC      { $$ = OrderBy_DESC;    }
    |       { $$ = OrderBy_DEFAULT; }
    ;    

tbName: IDENTIFIER;

colName: IDENTIFIER;
%%
