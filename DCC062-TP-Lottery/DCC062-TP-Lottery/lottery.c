/*
 *  lottery.c - Implementacao do algoritmo Lottery Scheduling e sua API
 *
 *  Autores: SUPER_PROGRAMADORES_C
 *  Projeto: Trabalho Pratico I - Sistemas Operacionais
 *  Organizacao: Universidade Federal de Juiz de Fora
 *  Departamento: Dep. Ciencia da Computacao
 *
 */

#include "lottery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Nome unico do algoritmo. Deve ter 4 caracteres.
const char lottName[] = "LOTT";

// Informações sobre o algoritmo de escalonamento Lottery
SchedInfo lottSchedInfo;

// Total de tickets de todos os processos READY
int totalTickets = 0;

//=====Funcoes Auxiliares=====

// Calcula o total de tickets de processos READY
int calcTotalTickets(Process *plist)
{
	Process *p;
	LotterySchedParams *lsp;
	int total = 0;

	for (p = plist; p != NULL; p = processGetNext(p))
	{
		// Só conta tickets de processos READY gerenciados pelo Lottery
		if (processGetStatus(p) == PROC_READY &&
			processGetSchedSlot(p) >= 0)
		{
			lsp = (LotterySchedParams *)processGetSchedParams(p);
			if (lsp != NULL)
			{
				total += lsp->num_tickets;
			}
		}
	}

	return total;
}

//=====Funcoes da API=====

// Funcao chamada pela inicializacao do S.O. para a incializacao do escalonador
// conforme o algoritmo Lottery Scheduling
// Deve envolver a inicializacao de possiveis parametros gerais
// Deve envolver o registro do algoritmo junto ao escalonador
void lottInitSchedInfo()
{
	// Inicializa o contador de tickets
	totalTickets = 0;

	// Inicializa a semente do gerador aleatório
	srand(time(NULL));

	// Preenche as informações do escalonador
	strcpy(lottSchedInfo.name, lottName);
	lottSchedInfo.initParamsFn = lottInitSchedParams;
	lottSchedInfo.notifyProcStatusChangeFn = lottNotifyProcStatusChange;
	lottSchedInfo.scheduleFn = lottSchedule;
	lottSchedInfo.releaseParamsFn = lottReleaseParams;

	// Registra o algoritmo no escalonador
	schedRegisterScheduler(&lottSchedInfo);
}

// Inicializa os parametros de escalonamento de um processo p, chamada
// normalmente quando o processo e' associado ao slot de Lottery
void lottInitSchedParams(Process *p, void *params)
{
	LotterySchedParams *lsp = (LotterySchedParams *)params;
	int slot;

	if (lsp == NULL)
	{
		// Se não há parâmetros, cria com valores padrão
		lsp = (LotterySchedParams *)malloc(sizeof(LotterySchedParams));
		lsp->num_tickets = 1; // Pelo menos 1 ticket
	}

	// Obtém o slot do escalonador
	slot = processGetSchedSlot(p);
	if (slot < 0)
	{
		// Se ainda não tem slot, assume slot 0 (primeiro slot registrado)
		slot = 0;
		// Associa o processo ao slot do Lottery
		processSetSchedSlot(p, slot);
	}

	lsp->slot = slot;

	// Associa os parâmetros ao processo
	processSetSchedParams(p, lsp);

	// Se o processo já está READY, adiciona seus tickets ao total
	if (processGetStatus(p) == PROC_READY)
	{
		totalTickets += lsp->num_tickets;
	}
}

// Recebe a notificação de que um processo sob gerência de Lottery mudou de estado
// Deve realizar qualquer atualização de dados da Loteria necessária quando um processo muda de estado
void lottNotifyProcStatusChange(Process *p)
{
	// Não precisa fazer nada aqui
	// O total de tickets é recalculado dinamicamente em lottSchedule()
	// quando necessário, evitando problemas com navegação de lista
}

// Retorna o proximo processo a obter a CPU, conforme o algortimo Lottery
Process *lottSchedule(Process *plist)
{
	Process *p;
	LotterySchedParams *lsp;
	int winningTicket, currentTicket = 0;

	// Recalcula total de tickets dos processos READY
	totalTickets = calcTotalTickets(plist);

	// Se não há tickets (nenhum processo READY), retorna NULL
	if (totalTickets <= 0)
	{
		return NULL;
	}

	// Sorteia um ticket vencedor entre 1 e totalTickets
	winningTicket = (rand() % totalTickets) + 1;

	// Percorre a lista de processos para encontrar o dono do ticket sorteado
	for (p = plist; p != NULL; p = processGetNext(p))
	{
		// Só considera processos READY gerenciados pelo Lottery
		if (processGetStatus(p) == PROC_READY &&
			processGetSchedSlot(p) >= 0)
		{
			lsp = (LotterySchedParams *)processGetSchedParams(p);
			if (lsp != NULL && lsp->num_tickets > 0)
			{
				currentTicket += lsp->num_tickets;

				// Se o ticket sorteado está na faixa deste processo, ele venceu
				if (winningTicket <= currentTicket)
				{
					return p;
				}
			}
		}
	}

	// Fallback: se não encontrou ninguém, retorna o primeiro READY
	for (p = plist; p != NULL; p = processGetNext(p))
	{
		if (processGetStatus(p) == PROC_READY &&
			processGetSchedSlot(p) >= 0)
		{
			return p;
		}
	}

	return NULL;
}

// Libera os parametros de escalonamento de um processo p, chamada
// normalmente quando o processo e' desassociado do slot de Lottery
// Retorna o numero do slot ao qual o processo estava associado
int lottReleaseParams(Process *p)
{
	LotterySchedParams *lsp;
	int slot;

	lsp = (LotterySchedParams *)processGetSchedParams(p);
	if (lsp == NULL)
		return -1;

	// Salva o slot antes de liberar
	slot = lsp->slot;

	// Se o processo está READY, remove seus tickets do total
	if (processGetStatus(p) == PROC_READY)
	{
		totalTickets -= lsp->num_tickets;
		if (totalTickets < 0)
			totalTickets = 0;
	}

	// Libera a memória dos parâmetros
	free(lsp);

	// Remove a associação do processo com os parâmetros
	processSetSchedParams(p, NULL);

	return slot;
}

// Transfere certo numero de tickets do processo src para o processo dst.
// Retorna o numero de tickets efetivamente transfeirdos (pode ser menos)
int lottTransferTickets(Process *src, Process *dst, int tickets)
{
	LotterySchedParams *lspSrc, *lspDst;
	int transferred;

	// Valida os processos
	if (src == NULL || dst == NULL || tickets <= 0)
	{
		return 0;
	}

	// Obtém os parâmetros dos processos
	lspSrc = (LotterySchedParams *)processGetSchedParams(src);
	lspDst = (LotterySchedParams *)processGetSchedParams(dst);

	if (lspSrc == NULL || lspDst == NULL)
	{
		return 0;
	}

	// Calcula quantos tickets podem ser transferidos
	// (não pode transferir mais do que o processo fonte tem,
	//  mas deve manter pelo menos 1 ticket no processo fonte)
	transferred = tickets;
	if (transferred > lspSrc->num_tickets - 1)
	{
		transferred = lspSrc->num_tickets - 1;
	}

	// Garante que não transfira valores negativos
	if (transferred <= 0)
	{
		return 0;
	}

	// Realiza a transferência
	lspSrc->num_tickets -= transferred;
	lspDst->num_tickets += transferred;

	// Não precisa atualizar totalTickets pois a soma continua a mesma
	// (apenas redistribui tickets entre processos)

	return transferred;
}
