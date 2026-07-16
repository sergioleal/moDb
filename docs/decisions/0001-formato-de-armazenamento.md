# ADR 0001 — Formato inicial de armazenamento

> ⚠️ **Parcialmente supersedida pelo pivô OO.** As decisões de storage físico
> (página de 4096 bytes, superbloco, little-endian explícito, validação) seguem
> válidas e foram reafirmadas pelas ADR-00X. As referências a metadados
> relacionais são superadas por [ADR-004](ADR-004-pagina-raiz-do-banco.md) e
> [ADR-005](ADR-005-mapa-de-identidade.md).

- Estado: superseção parcial (ver aviso acima); storage físico ainda aceito
- Data: 2026-07-13

## Contexto

O formato precisa sobreviver a mudanças de compilador, plataforma e layout dos
objetos C++. Também precisa rejeitar arquivos incompatíveis de forma segura.

## Decisão

- O banco usa um único arquivo dividido em páginas de 4096 bytes.
- A página zero é um superbloco e contém magic number, versão do formato,
  tamanho da página e referências aos metadados essenciais.
- Inteiros no arquivo são codificados explicitamente em little-endian.
- Nenhuma estrutura C++ é persistida com cópia direta de memória.
- Campos possuem largura fixa ou comprimento explicitamente codificado.
- Todo offset, tamanho, identificador e versão é validado antes do uso.
- O formato do MVP começa na versão `1`; versões desconhecidas são rejeitadas.
- Alterações incompatíveis exigem uma nova versão e uma estratégia explícita de
  migração ou uma mensagem de incompatibilidade.

## Consequências

Há mais código de serialização, mas o formato deixa de depender de padding, ABI,
endianness nativa ou detalhes do compilador. A página de 4096 bytes simplifica o
primeiro gerenciador de páginas; páginas configuráveis podem ser avaliadas após
o MVP.

