#include "caffe2/core/net.h"
#include "caffe2/utils/proto_utils.h"

#include "compiler.h"
#include "parser.h"

namespace caffe2 {
namespace script {

struct DefCompiler {
  DefCompiler(const Def& def, NetDef& net_def)
      : def(def), net_def_stack({&net_def}) {}
  void run() {
    cur().set_name(def.name().name());
    for (auto input : def.params()) {
      auto& name = input.ident().name();
      map(name, name);
      // cur().add_external_input(name);
    }
    for (auto output : def.returns()) {
      auto& name = output.ident().name();
      map(name, name);
      // cur().add_external_output(name);
    }
    emitStatements(def.statements());
  }
  void emitExpressionStatement(TreeRef stmt) {
    // expression with no used outputs
    auto r = emit(stmt);
    // remove the implicit single output
    r->clear_output();
  }
  void emitStatements(const ListView<TreeRef>& statements) {
    for (auto stmt : statements) {
      switch (stmt->kind()) {
        case TK_IF:
          emitIf(If(stmt));
          break;
        case TK_WHILE:
          emitWhile(While(stmt));
          break;
        case TK_ASSIGN:
          emitAssignment(Assign(stmt));
          break;
        default:
          emitExpressionStatement(stmt);
          break;
      }
    }
  }
  void map(const std::string& name, const std::string& value) {
    env[name] = value;
  }
  const std::string& lookup(const Ident& ident) {
    if (env.count(ident.name()) == 0)
      throw ErrorReport(ident) << "undefined value " << ident.name();
    return env[ident.name()];
  }
  void emitAssignment(const Assign& stmt) {
    OperatorDef* op;
    if (stmt.reduction() != '=') {
      if (stmt.idents().size() != 1) {
        throw ErrorReport(stmt)
            << "reductions are only allow when there is a single variable "
            << "on the left-hand side.";
      }
      auto lhs = stmt.idents()[0];
      auto expr =
          Compound::create(stmt.reduction(), stmt.range(), {lhs, stmt.rhs()});
      op = emit(expr);
    } else {
      op = emit(stmt.rhs());
    }
    while (op->output_size() < stmt.idents().size())
      op->add_output();
    int i = 0;
    for (auto ident : stmt.idents()) {
      std::string name = ident.name();
      // use of "_" gets renamed in Caffe2 graphs so that two uses
      // don't unintentionally interfere with each other
      if (name == "_") {
        name = fresh();
      }
      op->set_output(i++, name);
      map(ident.name(), name);
    }
  }
  void emitIf(const If& stmt) {
    auto cond = getValue(stmt.cond());
    auto op = cur().add_op();
    op->set_type("If");
    op->add_input(cond);
    auto true_branch = op->add_arg();
    true_branch->set_name("then_net");
    auto nd = true_branch->mutable_n();
    net_def_stack.push_back(nd);
    emitStatements(stmt.trueBranch());
    net_def_stack.pop_back();
    if (stmt.falseBranch().size() > 0) {
      auto false_branch = op->add_arg();
      false_branch->set_name("else_net");
      auto nd = false_branch->mutable_n();
      net_def_stack.push_back(nd);
      emitStatements(stmt.falseBranch());
      net_def_stack.pop_back();
    }
  }
  void emitWhile(const While& stmt) {
    std::string loop_var = fresh();
    emitConst(0, loop_var, "i"); // it needs a definition before loop
    auto op = cur().add_op();
    op->set_type("While");
    auto cond = op->add_arg();
    cond->set_name("cond_net");
    auto cond_net = cond->mutable_n();

    net_def_stack.push_back(cond_net);
    auto cond_op = emit(stmt.cond());
    cond_op->set_output(0, loop_var);
    net_def_stack.pop_back();

    op->add_input(loop_var);
    auto body = op->add_arg();
    body->set_name("loop_net");
    auto body_net = body->mutable_n();

    net_def_stack.push_back(body_net);
    emitStatements(stmt.body());
    net_def_stack.pop_back();
  }
  const std::string& getValue(const TreeRef& tree) {
    switch (tree->kind()) {
      case TK_IDENT:
        return lookup(Ident(tree));
      default:
        auto op = emit(tree);
        return op->output(0);
    }
  }
  std::string fresh() {
    return std::string("$t") + caffe2::to_string(next_fresh++);
  }
  const char* operatorName(int kind, int ninputs) {
    switch (kind) {
      case '+':
        return "Add";
      case '-':
        if (ninputs == 1)
          return "Negative";
        else
          return "Sub";
      case '*':
        return "Mul";
      case '/':
        return "Div";
      case TK_NE:
        return "NE";
      case TK_EQ:
        return "EQ";
      case '<':
        return "LT";
      case '>':
        return "GT";
      case TK_LE:
        return "LE";
      case TK_GE:
        return "GE";
      case TK_IF_EXPR:
        return "Conditional";
      case TK_AND:
        return "And";
      case TK_OR:
        return "Or";
      case TK_NOT:
        return "Not";
      default:
        throw std::runtime_error("unknown kind " + caffe2::to_string(kind));
    }
  }
  void fillArg(Argument* arg, const Attribute& attr) {
    std::string name = attr.name().name();
    arg->set_name(name);
    auto value = attr.value();
    // TODO: handle non-float attributes
    switch (value->kind()) {
      case TK_CONST: {
        auto v = value->tree(0)->doubleValue();
        auto f = value->tree(1)->stringValue();
        if (f == "f")
          arg->set_f(v);
        else
          arg->set_i(v);
      } break;
      case TK_LIST:
        for (auto t : value->trees()) {
          auto v = t->tree(0)->doubleValue();
          auto f = t->tree(1)->stringValue();
          if (f == "f")
            arg->add_floats(v);
          else
            arg->add_ints(v);
        }
        break;
    }
  }
  template <typename Trees>
  std::vector<std::string> getValues(const Trees& trees) {
    std::vector<std::string> result;
    for (const auto& tree : trees) {
      result.push_back(getValue(tree));
    }
    return result;
  }

  OperatorDef* emit(const TreeRef& tree) {
    switch (tree->kind()) {
      case TK_IDENT: {
        auto op = cur().add_op();
        op->set_type("Copy");
        op->add_input(lookup(Ident(tree)));
        op->add_output(fresh());
        return op;
      } break;
      case TK_NE:
      case TK_EQ:
      case '<':
      case '>':
      case TK_LE:
      case TK_GE:
      case '-':
      case '*':
      case '/':
      case '+':
      case TK_AND:
      case TK_OR:
      case TK_NOT:
      case TK_IF_EXPR: {
        // must be before add_op
        auto values = getValues(tree->trees());
        auto op = cur().add_op();
        op->set_type(operatorName(tree->kind(), tree->trees().size()));
        for (auto& v : values) {
          op->add_input(v);
        }
        op->add_output(fresh());
        auto broadcast = op->add_arg();
        broadcast->set_name("broadcast");
        broadcast->set_i(1);
        return op;
      }
      case TK_APPLY: {
        auto apply = Apply(tree);
        // Handle built-ins like zeros, ones, etc
        if (builtins.count(apply.name().name()) > 0) {
          return builtins[apply.name().name()](this, apply);
        }
        // must be before add_op
        auto values = getValues(apply.inputs());
        auto op = cur().add_op();
        op->set_type(apply.name().name());
        for (auto& v : values) {
          op->add_input(v);
        }
        // assume 1 output unless matched to more
        op->add_output(fresh());
        for (auto attribute : apply.attributes()) {
          fillArg(op->add_arg(), attribute);
        }
        return op;
      } break;
      case TK_CAST: {
        auto cast = Cast(tree);
        auto c2type = getType(cast.type());
        auto input = getValue(cast.input());
        auto op = cur().add_op();
        op->set_type("Cast");
        op->add_input(input);
        op->add_output(fresh());
        auto arg = op->add_arg();
        arg->set_name("to");
        arg->set_i(c2type);
        return op;
      } break;
      case TK_CONST: {
        return emitConst(
            tree->tree(0)->doubleValue(),
            fresh(),
            tree->tree(1)->stringValue());
      } break;
      default:
        throw ErrorReport(tree) << "NYI: " << tree;
        break;
    }
  }

  TensorProto_DataType getType(int type) {
    switch (type) {
      case TK_INT:
        return TensorProto_DataType_INT32;
      case TK_FLOAT:
        return TensorProto_DataType_FLOAT;
      case TK_LONG:
        return TensorProto_DataType_INT64;
      case TK_BOOL:
        return TensorProto_DataType_BOOL;
      default:
        throw std::runtime_error(
            "expected type token: " + caffe2::to_string(type));
    }
  }

  OperatorDef* emitConst(
      double v,
      const std::string& output,
      const std::string& type_ident) {
    auto op = cur().add_op();
    op->set_type("ConstantFill");
    auto dtype = op->add_arg();
    dtype->set_name("dtype");
    auto value = op->add_arg();
    value->set_name("value");
    if (type_ident == "f") {
      dtype->set_i(TensorProto_DataType_FLOAT);
      value->set_f(v);
    } else if (type_ident == "LL") {
      dtype->set_i(TensorProto_DataType_INT64);
      value->set_i(v);
    } else if (type_ident == "b") {
      dtype->set_i(TensorProto_DataType_BOOL);
      value->set_i(v != 0);
    } else if (type_ident == "i") {
      dtype->set_i(TensorProto_DataType_INT32);
      value->set_i(v);
    } else {
      throw std::runtime_error("unknown type_ident " + type_ident);
    }
    auto shape = op->add_arg();
    shape->set_name("shape");
    shape->add_ints(1);
    op->add_output(output);
    return op;
  }
  NetDef& cur() {
    return *net_def_stack.back();
  }
  const Def& def;
  std::unordered_map<std::string, std::string>
      env; // map from name in Def to name in NetDef
  std::vector<NetDef*> net_def_stack;
  int next_fresh = 0;

 private:
  OperatorDef* emitFillOp(const Apply& apply) {
    auto builtin_type = apply.name().name();
    auto values = getValues(apply.inputs());
    if (values.size() > 1) {
      throw ErrorReport(apply)
          << "Built-in " << builtin_type << " accepts 0 or 1 inputs.";
    }
    bool has_shape = false;
    for (const auto& attribute : apply.attributes()) {
      if (attribute.name().name() == "shape") {
        has_shape = true;
      } else {
        throw ErrorReport(apply)
            << "Unrecognized attribute " << attribute.name().name()
            << " for built-in " << builtin_type;
      }
    }
    if (builtin_type == "zeros" || builtin_type == "ones") {
      if ((values.size() != 1) && !has_shape) {
        throw ErrorReport(apply)
            << "Built-in " << builtin_type
            << " requires either 1 input or 1 shape attribute";
      }
    } else {
      // zeros_like or ones_like
      if (values.size() != 1) {
        throw ErrorReport(apply)
            << "Built-in " << builtin_type << " requires 1 input";
      }
    }

    auto op = cur().add_op();
    op->set_type("ConstantFill");
    if (values.size()) {
      op->add_input(values[0]);
      auto* input_as_shape = op->add_arg();
      input_as_shape->set_name("input_as_shape");
      if (builtin_type.find("_like") != std::string::npos) {
        // zeros_like, ones_like take the shape of the input as constant
        // tensor shape
        input_as_shape->set_i(0);
      } else {
        // zeros, ones take the values in the tensor as constant tensor
        // shape
        input_as_shape->set_i(1);
      }
    } else {
      fillArg(op->add_arg(), apply.attributes()[0]);
    }

    auto value = op->add_arg();
    value->set_name("value");
    if (builtin_type.find("ones") != std::string::npos) {
      value->set_f(1.0f);
    } else {
      value->set_f(0.0f);
    }
    op->add_output(fresh());
    return op;
  }

  std::unordered_map<
      std::string,
      std::function<OperatorDef*(DefCompiler*, const Apply&)>>
      builtins{{"zeros", &DefCompiler::emitFillOp},
               {"zeros_like", &DefCompiler::emitFillOp},
               {"ones", &DefCompiler::emitFillOp},
               {"ones_like", &DefCompiler::emitFillOp}};
};

struct CompilationUnitImpl {
  CompilationUnitImpl() {}
  void defineFunction(const Def& def) {
    if (functions.count(def.name().name()) > 0) {
      throw ErrorReport(def) << def.name().name() << " already defined.";
    }
    DefCompiler c(
        def, functions.emplace(def.name().name(), NetDef()).first->second);
    c.run();
  }
  void define(const std::string& str) {
    Parser p(str);
    while (p.lexer().cur().kind != TK_EOF) {
      defineFunction(Def(p.parseFunction()));
    }
  }
  std::unique_ptr<NetBase> createNet(Workspace* ws, const std::string& str) {
    if (functions.count(str) == 0)
      throw ErrorReport() << "undefined function: " << str << "\n";
    auto& net_def = functions[str];
    return caffe2::CreateNet(net_def, ws);
  }

 private:
  std::unordered_map<std::string, NetDef> functions;
};

CompilationUnit::CompilationUnit() : pImpl(new CompilationUnitImpl()) {}

void CompilationUnit::define(const std::string& str) {
  return pImpl->define(str);
}

std::unique_ptr<NetBase> CompilationUnit::createNet(
    Workspace* ws,
    const std::string& str) {
  return pImpl->createNet(ws, str);
}

CompilationUnit::~CompilationUnit() {}

} // namespace script
} // namespace caffe2
