# snovac — Compilador do Snovalang

`snovac` é o compilador oficial da linguagem **Snovalang**, implementado em **C11 puro**, totalmente autocontido, sem dependência do Rust, Cargo ou bibliotecas externas.

Este repositório é dedicado **exclusivamente ao desenvolvimento do compilador** da linguagem, incluindo front-end, análise semântica, inferência de tipos, máquina de estados de pattern matching e compilação para bytecode SnBC / executáveis nativos.

---

## 1. Arquitetura do Compilador

O pipeline do `snovac` é estruturado em fases desacopladas:

```
Fonte .snova 
   │
   ▼
[ Lexer ] ────────► Token Stream (token.h, lex.c, lex_literal.c, lex_token.c)
   │
   ▼
[ Parser ] ───────► AST em Arena (parse.c, parse_decl.c, parse_expr.c, parse_stmt.c, parse_type.c, ast.h)
   │
   ▼
[ Package Graph ] ─► Descoberta de raízes, resolução de pacotes e ciclos (package.c/.h)
   │
   ▼
[ Symbol Table ] ──► Escopos léxicos e interning de símbolos (intern.c/.h, symbol.c/.h)
   │
   ▼
[ Type System ] ───► Tipos canônicos com hash-consing & substituição de genéricos (types.c/.h, builtins.c/.h)
   │
   ▼
[ Resolver ] ──────► Ligação de identificadores, imports e prelúdio implícito (resolve.c/.h)
   │
   ▼
[ Type Checker ] ──► Verificação semântica, match exaustivo conciso e diagnósticos SNOVA* (check.c/.h)
   │
   ├─► [ Evaluator / Interpreter ] ──► Execução direta da AST (eval.c, eval_expr.c, eval_stmt.c, eval_string.c)
   │
   └─► [ Bytecode & Native Build ] ──► Emissor SnBC, VM e gerador de executáveis autônomos (snbc.c, emit_bc.c, vm.c, value.c, link_append.c)
```

---

## 2. Mapa de Módulos

| Módulo | Responsabilidade |
|---|---|
| `arena.c/.h` | Alocador por arena para todo o ciclo de vida da AST e tabelas. |
| `diag.c/.h` | Diagnósticos padronizados `SNOVAxxxx` com spans precisos, linhas de contexto e carets. |
| `token.h`, `lex*.c` | Lexer UTF-8 com suporte a números sufixados, interpolação `${}`, docstrings e palavras-chave contextuais. |
| `ast.c/.h`, `parse*.c` | Parser recursivo-descendente + algoritmo de Pratt para expressões com precedência e associatividade. |
| `intern.c/.h` | Tabela de interning FNV-1a para comparação de strings por ponteiro. |
| `symbol.c/.h` | Escopos encadeados e símbolos tipados. |
| `package.c/.h` | Varredura de diretórios, parsing de cabeçalhos de pacotes, detecção de ciclos em grafo. |
| `types.c/.h` | Tipos canônicos com hash-consing e substituição de parâmetros de tipo genéricos (`sn_type_subst_names`). |
| `builtins.c/.h` | Definição dos tipos primitivos, `Array<T>`, e métodos de tipos básicos. |
| `resolve.c/.h` | Resolução de escopo em 5 níveis, imports qualificados e prelúdio implícito. |
| `check.c/.h` | Type-checker estrito, inferência de lambdas, substituição de genéricos em métodos e verificação concisa de match exaustivo. |
| `eval*.c` | Interpretador de AST para execução em tempo de desenvolvimento (`snovac run`). |
| `snbc.c/.h` | Formato e manipulador de chunks de Bytecode Snovalang (SnBC). |
| `value.c/.h` | Modelo de valores de runtime com contagem de referências atômica. |
| `vm.c/.h` | Máquina Virtual de pilha para execução de bytecode SnBC. |
| `emit_bc.c/.h` | Compilador AST $\rightarrow$ Bytecode SnBC. |
| `link_append.c/.h` | Construtor de executáveis nativos independentes. |
| `main.c` | CLI e driver de comandos (`build`, `run`, `check`, `--emit=tokens`, `--emit=ast`). |

---

## 3. Como Construir e Testar

### Comandos de Build

```bash
# Compilar o compilador snovac
make

# Executar a suíte de testes unitários em C (164 asserções)
make unit

# Executar os testes de conformidade léxica e sintática (100% do corpus)
make conformance

# Executar toda a suíte de testes
make test

# Limpar artefatos de compilação
make clean
```

---

## 4. Uso da CLI

```bash
# Verificar versão
./build/snovac --version

# Compilar um programa Snovalang para executável nativo autônomo
./build/snovac build app.snova -o app
./app

# Executar diretamente o programa via AST / interpretador
./build/snovac run app.snova

# Resolução semântica e type-checking completo
./build/snovac check app.snova

# Resolução semântica em nível de projeto
./build/snovac check --project src/Main.snova

# Exibir os tokens de um arquivo
./build/snovac --emit=tokens app.snova

# Exibir a árvore sintática (AST)
./build/snovac --emit=ast app.snova
```
